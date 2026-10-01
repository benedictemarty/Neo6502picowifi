/* tnfs_client.c — voir tnfs_client.h. */
#include "tnfs_client.h"

#include <string.h>

enum {
    CMD_MOUNT = 0x00, CMD_UMOUNT = 0x01,
    CMD_CLOSEDIR = 0x12, CMD_MKDIR = 0x13, CMD_RMDIR = 0x14,
    CMD_OPENDIRX = 0x17, CMD_READDIRX = 0x18,
    CMD_READ = 0x21, CMD_WRITE = 0x22, CMD_CLOSE = 0x23, CMD_STAT = 0x24,
    CMD_LSEEK = 0x25, CMD_UNLINK = 0x26, CMD_RENAME = 0x28, CMD_OPEN = 0x29,
};

#define DIRENTRY_DIR   0x01
#define DIRSTATUS_EOF  0x01
#define S_IFMT_        0170000
#define S_IFDIR_       0040000

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

void tnfs_init(struct tnfs_client *c, tnfs_xfer_fn xfer, tnfs_sleep_fn sleep, void *ctx)
{
    memset(c, 0, sizeof *c);
    c->xfer = xfer;
    c->sleep = sleep;
    c->ctx = ctx;
}

/* Requête en cours de construction */
struct req {
    uint8_t b[TNFS_MSG_MAX];
    size_t  n;
    bool    overflow;
};

static void req_start(struct tnfs_client *c, struct req *r, uint8_t cmd)
{
    wr16(r->b, c->conn);
    r->b[2] = 0;                       /* numéro de séquence posé à l'envoi */
    r->b[3] = cmd;
    r->n = 4;
    r->overflow = false;
}

static void put8(struct req *r, uint8_t v)
{
    if (r->n + 1 > sizeof r->b) { r->overflow = true; return; }
    r->b[r->n++] = v;
}

static void put_bytes(struct req *r, const void *d, size_t n)
{
    if (r->n + n > sizeof r->b) { r->overflow = true; return; }
    memcpy(r->b + r->n, d, n);
    r->n += n;
}

static void put_str(struct req *r, const char *s)
{
    put_bytes(r, s, strlen(s) + 1);
}

/* Envoie la requête et attend la réponse correspondante (session, séquence,
   commande) ; gère les répétitions et EAGAIN. Renvoie le statut (octet 4,
   ou 0 pour UMOUNT sans statut), ou une erreur locale. La réponse est dans
   c->resp / c->resp_len. */
static int transact(struct tnfs_client *c, struct req *r)
{
    if (r->overflow) return TNFS_ERR_ARG;
    uint8_t cmd = r->b[3];
    r->b[2] = ++c->seq;
    uint32_t wait = c->min_retry_ms > TNFS_TRY_MS ? c->min_retry_ms : TNFS_TRY_MS;
    for (int attempt = 0, again = 0; attempt < TNFS_TRIES; attempt++) {
        int n = c->xfer(c->ctx, r->b, r->n, c->resp, sizeof c->resp, wait);
        /* datagrammes en retard (autre séquence) : on continue d'écouter sans
           renvoyer, 16 au plus (un serveur bavard ne bloque pas le modem) */
        for (int stale = 0; n >= 4 && stale < 16 && (c->resp[2] != r->b[2] || c->resp[3] != cmd
                            || (cmd != CMD_MOUNT && rd16(c->resp) != c->conn)); stale++)
            n = c->xfer(c->ctx, NULL, 0, c->resp, sizeof c->resp, wait);
        if (n >= 4 && (c->resp[2] != r->b[2] || c->resp[3] != cmd)) n = -1;
        if (n < 4) continue;                               /* rien : on renvoie */
        c->resp_len = (size_t)n;
        if (n < 5) return TNFS_ERR_PROTO;
        if (c->resp[4] == TNFS_EAGAIN && n >= 7 && again < 8) {
            /* le serveur demande d'attendre : nouvelle séquence après le délai */
            if (c->sleep) c->sleep(c->ctx, rd16(c->resp + 5));
            r->b[2] = ++c->seq;
            again++;
            attempt = -1;
            continue;
        }
        return c->resp[4];
    }
    return TNFS_ERR_TIMEOUT;
}

static int need_mount(const struct tnfs_client *c) { return c->mounted ? 0 : TNFS_ERR_MOUNT; }

int tnfs_mount(struct tnfs_client *c, const char *path, const char *user, const char *pass)
{
    struct req r;
    c->conn = 0;
    c->mounted = false;
    req_start(c, &r, CMD_MOUNT);
    put8(&r, 0x02);                     /* version 1.2 : mineur puis majeur */
    put8(&r, 0x01);
    put_str(&r, path && *path ? path : "/");
    put_str(&r, user ? user : "");
    put_str(&r, pass ? pass : "");
    int st = transact(c, &r);
    if (st != TNFS_OK) return st;
    if (c->resp_len < 9) return TNFS_ERR_PROTO;
    c->conn = rd16(c->resp);
    c->version = (uint16_t)(c->resp[6] << 8 | c->resp[5]);
    c->min_retry_ms = rd16(c->resp + 7);
    c->mounted = true;
    return TNFS_OK;
}

int tnfs_umount(struct tnfs_client *c)
{
    if (!c->mounted) return TNFS_OK;
    struct req r;
    req_start(c, &r, CMD_UMOUNT);
    int st = transact(c, &r);
    c->mounted = false;                  /* session considérée close dans tous les cas */
    return st;
}

static int path_cmd(struct tnfs_client *c, uint8_t cmd, const char *path)
{
    int e = need_mount(c);
    if (e) return e;
    if (strlen(path) > TNFS_PATH_MAX) return TNFS_ERR_ARG;
    struct req r;
    req_start(c, &r, cmd);
    put_str(&r, path);
    return transact(c, &r);
}

int tnfs_open(struct tnfs_client *c, const char *path, uint16_t flags, uint16_t mode, uint8_t *fd)
{
    int e = need_mount(c);
    if (e) return e;
    if (strlen(path) > TNFS_PATH_MAX) return TNFS_ERR_ARG;
    struct req r;
    uint8_t v[4];
    req_start(c, &r, CMD_OPEN);
    wr16(v, flags);
    wr16(v + 2, mode);
    put_bytes(&r, v, 4);
    put_str(&r, path);
    int st = transact(c, &r);
    if (st != TNFS_OK) return st;
    if (c->resp_len < 6) return TNFS_ERR_PROTO;
    *fd = c->resp[5];
    return TNFS_OK;
}

int tnfs_read(struct tnfs_client *c, uint8_t fd, uint8_t *buf, size_t n, size_t *got)
{
    *got = 0;
    int e = need_mount(c);
    if (e) return e;
    if (n == 0 || n > TNFS_IO_MAX) return TNFS_ERR_ARG;
    struct req r;
    uint8_t v[2];
    req_start(c, &r, CMD_READ);
    put8(&r, fd);
    wr16(v, (uint16_t)n);
    put_bytes(&r, v, 2);
    int st = transact(c, &r);
    if (st != TNFS_OK) return st;
    if (c->resp_len < 7) return TNFS_ERR_PROTO;
    size_t k = rd16(c->resp + 5);
    if (k > n || 7 + k > c->resp_len) return TNFS_ERR_PROTO;
    memcpy(buf, c->resp + 7, k);
    *got = k;
    return TNFS_OK;
}

int tnfs_write(struct tnfs_client *c, uint8_t fd, const uint8_t *buf, size_t n, size_t *written)
{
    *written = 0;
    int e = need_mount(c);
    if (e) return e;
    if (n == 0 || n > TNFS_IO_MAX) return TNFS_ERR_ARG;
    struct req r;
    uint8_t v[2];
    req_start(c, &r, CMD_WRITE);
    put8(&r, fd);
    wr16(v, (uint16_t)n);
    put_bytes(&r, v, 2);
    put_bytes(&r, buf, n);
    int st = transact(c, &r);
    if (st != TNFS_OK) return st;
    if (c->resp_len < 7) return TNFS_ERR_PROTO;
    *written = rd16(c->resp + 5);
    return *written <= n ? TNFS_OK : TNFS_ERR_PROTO;
}

int tnfs_close(struct tnfs_client *c, uint8_t fd)
{
    int e = need_mount(c);
    if (e) return e;
    struct req r;
    req_start(c, &r, CMD_CLOSE);
    put8(&r, fd);
    return transact(c, &r);
}

int tnfs_lseek(struct tnfs_client *c, uint8_t fd, int32_t off, uint8_t whence, uint32_t *pos)
{
    int e = need_mount(c);
    if (e) return e;
    if (whence > 2) return TNFS_ERR_ARG;
    struct req r;
    uint8_t v[4];
    req_start(c, &r, CMD_LSEEK);
    put8(&r, fd);
    put8(&r, whence);
    wr32(v, (uint32_t)off);
    put_bytes(&r, v, 4);
    int st = transact(c, &r);
    if (st == TNFS_OK && pos && c->resp_len >= 9) *pos = rd32(c->resp + 5);
    return st;
}

int tnfs_stat(struct tnfs_client *c, const char *path, struct tnfs_stat *s)
{
    int st = path_cmd(c, CMD_STAT, path);
    if (st != TNFS_OK) return st;
    /* statut, mode, uid, gid, taille, atime, mtime, ctime */
    if (c->resp_len < 5 + 2 + 2 + 2 + 4 + 4 + 4 + 4) return TNFS_ERR_PROTO;
    s->mode = rd16(c->resp + 5);
    s->size = rd32(c->resp + 11);
    s->mtime = rd32(c->resp + 19);
    s->is_dir = (s->mode & S_IFMT_) == S_IFDIR_;
    return TNFS_OK;
}

int tnfs_list(struct tnfs_client *c, const char *path, tnfs_dir_cb cb, void *ctx)
{
    int e = need_mount(c);
    if (e) return e;
    if (strlen(path) > TNFS_PATH_MAX) return TNFS_ERR_ARG;
    struct req r;
    req_start(c, &r, CMD_OPENDIRX);
    put8(&r, 0);                        /* options par défaut : dossiers d'abord, sans cachés */
    put8(&r, 0);                        /* tri par nom                                        */
    put8(&r, 0); put8(&r, 0);           /* nombre de résultats illimité                       */
    put8(&r, 0);                        /* pas de motif                                       */
    put_str(&r, path);
    int st = transact(c, &r);
    if (st != TNFS_OK) return st;
    if (c->resp_len < 6) return TNFS_ERR_PROTO;
    uint8_t dh = c->resp[5];
    bool more = true, stop = false;
    while (more && !stop) {
        req_start(c, &r, CMD_READDIRX);
        put8(&r, dh);
        put8(&r, 0);                    /* autant d'entrées que tient la réponse */
        st = transact(c, &r);
        if (st == TNFS_EEOF) { st = TNFS_OK; break; }
        if (st != TNFS_OK) break;
        if (c->resp_len < 10) { st = TNFS_ERR_PROTO; break; }
        unsigned count = c->resp[5];
        more = !(c->resp[6] & DIRSTATUS_EOF);
        size_t p = 9;                   /* statut, count, dirstatus, dirpos (2) */
        for (unsigned i = 0; i < count && !stop; i++) {
            if (p + 13 > c->resp_len) { st = TNFS_ERR_PROTO; more = false; break; }
            struct tnfs_dirent d;
            d.is_dir = c->resp[p] & DIRENTRY_DIR;
            d.size = rd32(c->resp + p + 1);
            d.mtime = rd32(c->resp + p + 5);
            p += 13;                    /* flags, size, mtime, ctime */
            const uint8_t *end = memchr(c->resp + p, 0, c->resp_len - p);
            if (!end) { st = TNFS_ERR_PROTO; more = false; break; }
            size_t nl = (size_t)(end - (c->resp + p));
            if (nl > TNFS_PATH_MAX) nl = TNFS_PATH_MAX;
            memcpy(d.name, c->resp + p, nl);
            d.name[nl] = 0;
            p = (size_t)(end - c->resp) + 1;
            if (!cb(ctx, &d)) stop = true;
        }
    }
    req_start(c, &r, CMD_CLOSEDIR);
    put8(&r, dh);
    int cst = transact(c, &r);
    return st != TNFS_OK ? st : cst;
}

int tnfs_unlink(struct tnfs_client *c, const char *path) { return path_cmd(c, CMD_UNLINK, path); }
int tnfs_mkdir(struct tnfs_client *c, const char *path) { return path_cmd(c, CMD_MKDIR, path); }
int tnfs_rmdir(struct tnfs_client *c, const char *path) { return path_cmd(c, CMD_RMDIR, path); }

int tnfs_rename(struct tnfs_client *c, const char *from, const char *to)
{
    int e = need_mount(c);
    if (e) return e;
    if (strlen(from) > TNFS_PATH_MAX || strlen(to) > TNFS_PATH_MAX) return TNFS_ERR_ARG;
    struct req r;
    req_start(c, &r, CMD_RENAME);
    put_str(&r, from);
    put_str(&r, to);
    return transact(c, &r);
}

const char *tnfs_strerror(int code)
{
    static const char *const names[] = {
        "OK", "EPERM", "ENOENT", "EIO", "ENXIO", "E2BIG", "EBADF", "EAGAIN", "ENOMEM",
        "EACCES", "EBUSY", "EEXIST", "ENOTDIR", "EISDIR", "EINVAL", "ENFILE", "EMFILE",
        "EFBIG", "ENOSPC", "ESPIPE", "EROFS", "ENAMETOOLONG", "ENOSYS", "ENOTEMPTY",
        "ELOOP", "ENODATA", "ENOSTR", "EPROTO", "EBADFD", "EUSERS", "ENOBUFS",
        "EALREADY", "ESTALE", "EOF",
    };
    switch (code) {
    case TNFS_ERR_TIMEOUT: return "TIMEOUT";
    case TNFS_ERR_PROTO:   return "BADREPLY";
    case TNFS_ERR_MOUNT:   return "NOTMOUNTED";
    case TNFS_ERR_ARG:     return "BADARG";
    default:
        if (code >= 0 && code < (int)(sizeof names / sizeof names[0])) return names[code];
        return "UNKNOWN";
    }
}
