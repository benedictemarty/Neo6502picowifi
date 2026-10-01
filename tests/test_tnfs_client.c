/*
 * test_tnfs_client.c — client TNFS (US-T16) : tests sur PC.
 *  1. maquette de transport : réponse perdue (même séquence renvoyée), réponse
 *     en retard ignorée, EAGAIN, délai dépassé ;
 *  2. si TNFS_HOST est défini : session réelle contre tnfsd (port 16384) —
 *     écriture, relecture, déplacement, stat, liste, renommage, suppression.
 */
#define _GNU_SOURCE
#include "../src/tnfs_client.h"

#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------------------------------------- maquette */

static struct {
    int sends, listens, drop_first, stale, eagain, never, slept;
    uint8_t last_req[TNFS_MSG_MAX];
    size_t last_len;
} F;

static size_t reply(const uint8_t *req, uint8_t *resp, uint8_t status, const uint8_t *extra, size_t n)
{
    resp[0] = 0xEF; resp[1] = 0xBE;                     /* session 0xBEEF */
    resp[2] = req[2]; resp[3] = req[3]; resp[4] = status;
    if (n) memcpy(resp + 5, extra, n);
    return 5 + n;
}

static int fake_xfer(void *ctx, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, uint32_t t)
{
    (void)ctx; (void)cap; (void)t;
    if (req) { F.sends++; memcpy(F.last_req, req, len); F.last_len = len; }
    else F.listens++;
    if (F.never) return -1;
    if (req && F.drop_first) { F.drop_first--; return -1; }
    if (req && F.stale) {                               /* d'abord une vieille réponse */
        F.stale--;
        uint8_t old[4] = { F.last_req[0], F.last_req[1], (uint8_t)(F.last_req[2] - 1), F.last_req[3] };
        return (int)reply(old, resp, 0, NULL, 0);
    }
    const uint8_t *q = F.last_req;
    if (F.eagain) { F.eagain--; uint8_t b[2] = { 50, 0 }; return (int)reply(q, resp, 0x07, b, 2); }
    switch (q[3]) {
    case 0x00: { uint8_t v[4] = { 0x06, 0x02, 0x10, 0x00 }; return (int)reply(q, resp, 0, v, 4); }  /* 2.6, 16 ms */
    case 0x21: { uint8_t v[5] = { 3, 0, 'a', 'b', 'c' }; return (int)reply(q, resp, 0, v, 5); }
    default: return (int)reply(q, resp, 0, NULL, 0);
    }
}

static void fake_sleep(void *ctx, uint32_t ms) { (void)ctx; F.slept += (int)ms; }

static void test_fake(void)
{
    struct tnfs_client c;
    uint8_t buf[16];
    size_t got;
    memset(&F, 0, sizeof F);
    tnfs_init(&c, fake_xfer, fake_sleep, NULL);
    CHECK(tnfs_read(&c, 1, buf, 4, &got) == TNFS_ERR_MOUNT);
    CHECK(tnfs_mount(&c, "/", NULL, NULL) == TNFS_OK);
    CHECK(c.mounted && c.conn == 0xBEEF && c.version == 0x0206 && c.min_retry_ms == 16);
    CHECK(F.last_req[3] == 0x00 && F.last_req[4] == 0x02 && F.last_req[5] == 0x01);   /* version 1.2 */

    /* réponse perdue : même requête, même séquence */
    F.sends = 0; F.drop_first = 1;
    CHECK(tnfs_read(&c, 1, buf, 3, &got) == TNFS_OK && got == 3 && !memcmp(buf, "abc", 3));
    CHECK(F.sends == 2);
    uint8_t seq = F.last_req[2];
    /* réponse en retard (séquence précédente) : écoutée puis ignorée, sans renvoi */
    F.sends = F.listens = 0; F.stale = 1;
    CHECK(tnfs_read(&c, 1, buf, 3, &got) == TNFS_OK && F.sends == 1 && F.listens == 1);
    CHECK(F.last_req[2] == (uint8_t)(seq + 1));
    /* EAGAIN : attente demandée, nouvelle séquence */
    F.eagain = 2;
    CHECK(tnfs_close(&c, 1) == TNFS_OK && F.slept == 100);
    /* aucun serveur : délai dépassé après TNFS_TRIES envois */
    F.sends = 0; F.never = 1;
    CHECK(tnfs_close(&c, 1) == TNFS_ERR_TIMEOUT && F.sends == TNFS_TRIES);
    F.never = 0;
    /* arguments */
    char longp[400]; memset(longp, 'p', sizeof longp - 1); longp[sizeof longp - 1] = 0;
    CHECK(tnfs_unlink(&c, longp) == TNFS_ERR_ARG);
    CHECK(tnfs_read(&c, 1, buf, TNFS_IO_MAX + 1, &got) == TNFS_ERR_ARG);
    CHECK(tnfs_lseek(&c, 1, 0, 3, NULL) == TNFS_ERR_ARG);
    CHECK(!strcmp(tnfs_strerror(TNFS_ENOENT), "ENOENT") && !strcmp(tnfs_strerror(TNFS_EEOF), "EOF")
          && !strcmp(tnfs_strerror(TNFS_ERR_TIMEOUT), "TIMEOUT"));
    CHECK(tnfs_umount(&c) == TNFS_OK && !c.mounted);
}

/* -------------------------------------------------- tnfsd réel */

static int sock = -1;

static int udp_xfer(void *ctx, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, uint32_t t)
{
    (void)ctx;
    if (req && send(sock, req, len, 0) != (ssize_t)len) return -1;
    struct pollfd p = { sock, POLLIN, 0 };
    if (poll(&p, 1, (int)t) <= 0) return -1;
    return (int)recv(sock, resp, cap, 0);
}

struct listing { int n, dirs; bool saw_file; uint32_t file_size; };
static bool on_entry(void *ctx, const struct tnfs_dirent *e)
{
    struct listing *l = ctx;
    l->n++;
    l->dirs += e->is_dir;
    if (!strcmp(e->name, "essai.bin")) { l->saw_file = true; l->file_size = e->size; }
    return true;
}

static void test_real(const char *host)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(16384) };
    inet_pton(AF_INET, host, &a.sin_addr);
    connect(sock, (struct sockaddr *)&a, sizeof a);
    struct tnfs_client c;
    tnfs_init(&c, udp_xfer, NULL, NULL);
    int r = tnfs_mount(&c, "/", NULL, NULL);
    CHECK(r == TNFS_OK);
    if (r != TNFS_OK) { printf("tnfsd injoignable sur %s:16384 (%s)\n", host, tnfs_strerror(r)); return; }
    printf("tnfsd : version %u.%u, session 0x%04x\n", c.version >> 8, c.version & 0xff, c.conn);

    uint8_t fd, data[1500], back[1500];
    size_t n;
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 13 + 7);
    tnfs_unlink(&c, "/essai.bin");
    tnfs_rmdir(&c, "/dossier");
    CHECK(tnfs_open(&c, "/essai.bin", TNFS_O_WRONLY | TNFS_O_CREAT | TNFS_O_TRUNC, 0644, &fd) == TNFS_OK);
    for (size_t off = 0; off < sizeof data; off += n) {                 /* 512 + 512 + 476 */
        size_t k = sizeof data - off < TNFS_IO_MAX ? sizeof data - off : TNFS_IO_MAX;
        CHECK(tnfs_write(&c, fd, data + off, k, &n) == TNFS_OK && n == k);
    }
    CHECK(tnfs_close(&c, fd) == TNFS_OK);

    struct tnfs_stat st;
    CHECK(tnfs_stat(&c, "/essai.bin", &st) == TNFS_OK && st.size == sizeof data && !st.is_dir && st.mtime > 0);
    CHECK(tnfs_stat(&c, "/absent", &st) == TNFS_ENOENT);

    CHECK(tnfs_open(&c, "/essai.bin", TNFS_O_RDONLY, 0, &fd) == TNFS_OK);
    size_t total = 0;
    int rr;
    while ((rr = tnfs_read(&c, fd, back + total, TNFS_IO_MAX, &n)) == TNFS_OK) total += n;
    CHECK(rr == TNFS_EEOF && total == sizeof data && !memcmp(back, data, sizeof data));
    uint32_t pos = 0;
    CHECK(tnfs_lseek(&c, fd, 1000, 0, &pos) == TNFS_OK && pos == 1000);
    CHECK(tnfs_read(&c, fd, back, 10, &n) == TNFS_OK && n == 10 && !memcmp(back, data + 1000, 10));
    CHECK(tnfs_lseek(&c, fd, -5, 2, &pos) == TNFS_OK && pos == sizeof data - 5);
    CHECK(tnfs_close(&c, fd) == TNFS_OK);
    CHECK(tnfs_close(&c, fd) == 0x06);                                   /* EBADF */

    CHECK(tnfs_mkdir(&c, "/dossier") == TNFS_OK);
    struct listing l = { 0 };
    CHECK(tnfs_list(&c, "/", on_entry, &l) == TNFS_OK);
    CHECK(l.saw_file && l.file_size == sizeof data && l.dirs >= 1);
    CHECK(tnfs_rename(&c, "/essai.bin", "/dossier/renomme.bin") == TNFS_OK);
    CHECK(tnfs_stat(&c, "/dossier/renomme.bin", &st) == TNFS_OK && st.size == sizeof data);
    CHECK(tnfs_rmdir(&c, "/dossier") != TNFS_OK);                       /* pas vide */
    CHECK(tnfs_unlink(&c, "/dossier/renomme.bin") == TNFS_OK);
    CHECK(tnfs_rmdir(&c, "/dossier") == TNFS_OK);
    CHECK(tnfs_open(&c, "/absent", TNFS_O_RDONLY, 0, &fd) == TNFS_ENOENT);
    CHECK(tnfs_umount(&c) == TNFS_OK);
    close(sock);
}

int main(void)
{
    test_fake();
    const char *host = getenv("TNFS_HOST");
    if (host) test_real(host);
    else printf("*** partie tnfsd réel SAUTÉE : définir TNFSD (binaire tnfsd) pour make -C tests ***\n");
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
