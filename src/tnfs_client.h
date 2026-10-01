/*
 * tnfs_client.h — client TNFS (US-T16) : fichiers distants pour tout
 * programme Neo6502, via les commandes AT+N… du modem.
 *
 * D'après la spécification TNFS (spectranet/tnfs/tnfs-protocol.md, 2020-07-15)
 * et les constantes du serveur de référence tnfsd (MAXMSGSZ 532, MAX_IOSZ 512,
 * SEEK 0/1/2, DIRENTRY_DIR 0x01, DIRSTATUS_EOF 0x01). Portable : le transport
 * est une fonction « requête → réponse » fournie par l'appelant (UDP sur le
 * Pico ou le PC, maquette en test). Une seule requête en vol ; sur délai
 * dépassé, la même requête est renvoyée avec le même numéro de séquence (le
 * serveur renvoie alors sa dernière réponse).
 */
#ifndef TNFS_CLIENT_H
#define TNFS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TNFS_MSG_MAX     532      /* datagramme TNFS maximal (tnfsd MAXMSGSZ) */
#define TNFS_IO_MAX      512      /* données d'un READ/WRITE (tnfsd MAX_IOSZ) */
#define TNFS_PATH_MAX    255
#define TNFS_TRIES       4        /* envoi initial + 3 répétitions           */
#define TNFS_TRY_MS      1500     /* attente d'une réponse, par essai        */

/* Codes de retour : 0 = succès, 1..0x21 = codes TNFS du serveur (0x21 = EOF),
   valeurs négatives = erreurs locales. */
#define TNFS_OK           0
#define TNFS_ENOENT       0x02
#define TNFS_EAGAIN       0x07
#define TNFS_EEOF         0x21
#define TNFS_ERR_TIMEOUT  (-1)    /* pas de réponse après TNFS_TRIES essais  */
#define TNFS_ERR_PROTO    (-2)    /* réponse mal formée                      */
#define TNFS_ERR_MOUNT    (-3)    /* pas de session montée                   */
#define TNFS_ERR_ARG      (-4)    /* argument invalide (chemin trop long…)   */

/* Ouverture (tnfsd tnfs_file.h) */
#define TNFS_O_RDONLY  0x0001
#define TNFS_O_WRONLY  0x0002
#define TNFS_O_RDWR    0x0003
#define TNFS_O_APPEND  0x0008
#define TNFS_O_CREAT   0x0100
#define TNFS_O_TRUNC   0x0200

/* Transport : envoie req (sauf si req == NULL : écoute seulement), attend au
   plus timeout_ms un datagramme, le copie dans resp ; renvoie sa longueur, ou
   < 0 si rien n'est arrivé. Des réponses en retard peuvent arriver : le
   client les filtre (session, séquence, commande). */
typedef int (*tnfs_xfer_fn)(void *ctx, const uint8_t *req, size_t len,
                            uint8_t *resp, size_t cap, uint32_t timeout_ms);
/* Attente (réponse EAGAIN du serveur) ; peut être NULL. */
typedef void (*tnfs_sleep_fn)(void *ctx, uint32_t ms);

struct tnfs_client {
    tnfs_xfer_fn  xfer;
    tnfs_sleep_fn sleep;
    void         *ctx;
    bool          mounted;
    uint16_t      conn;          /* identifiant de session donné par MOUNT */
    uint8_t       seq;
    uint16_t      version;       /* version du serveur (majeur << 8 | mineur) */
    uint16_t      min_retry_ms;
    uint8_t       resp[TNFS_MSG_MAX];
    size_t        resp_len;
};

struct tnfs_stat {
    uint16_t mode;
    uint32_t size, mtime;
    bool     is_dir;
};

struct tnfs_dirent {
    bool     is_dir;
    uint32_t size, mtime;
    char     name[TNFS_PATH_MAX + 1];
};

void tnfs_init(struct tnfs_client *c, tnfs_xfer_fn xfer, tnfs_sleep_fn sleep, void *ctx);

int tnfs_mount(struct tnfs_client *c, const char *path, const char *user, const char *pass);
int tnfs_umount(struct tnfs_client *c);

int tnfs_open(struct tnfs_client *c, const char *path, uint16_t flags, uint16_t mode, uint8_t *fd);
int tnfs_read(struct tnfs_client *c, uint8_t fd, uint8_t *buf, size_t n, size_t *got);
int tnfs_write(struct tnfs_client *c, uint8_t fd, const uint8_t *buf, size_t n, size_t *written);
int tnfs_close(struct tnfs_client *c, uint8_t fd);
/* whence : 0 début, 1 position courante, 2 fin ; *pos = nouvelle position
   (si le serveur la renvoie, version > 1.0 ; sinon inchangé). */
int tnfs_lseek(struct tnfs_client *c, uint8_t fd, int32_t off, uint8_t whence, uint32_t *pos);
int tnfs_stat(struct tnfs_client *c, const char *path, struct tnfs_stat *st);

/* Liste de dossier par OPENDIRX/READDIRX (triée, dossiers d'abord, sans
   fichiers cachés) ; cb appelé pour chaque entrée, false pour arrêter. */
typedef bool (*tnfs_dir_cb)(void *ctx, const struct tnfs_dirent *e);
int tnfs_list(struct tnfs_client *c, const char *path, tnfs_dir_cb cb, void *ctx);

int tnfs_unlink(struct tnfs_client *c, const char *path);
int tnfs_mkdir(struct tnfs_client *c, const char *path);
int tnfs_rmdir(struct tnfs_client *c, const char *path);
int tnfs_rename(struct tnfs_client *c, const char *from, const char *to);

/* Nom court d'un code (ENOENT, EOF, TIMEOUT…) pour les messages. */
const char *tnfs_strerror(int code);

#endif
