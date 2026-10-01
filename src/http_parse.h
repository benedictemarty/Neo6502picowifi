/*
 * http_parse.h — analyse HTTP du mode flux (US-T11) : URL, redirections,
 * en-têtes de réponse, corps « chunked ». Portable, sans allocation ; testé
 * sur PC (tests/test_http_parse.c). La requête et le transport sont dans
 * at_modem.c (AT+HTTPGET / AT+HTTPREAD).
 */
#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTP_HOST_MAX  64
#define HTTP_PATH_MAX  240
#define HTTP_HDR_MAX   2048    /* en-têtes de réponse acceptés */
#define HTTP_TYPE_MAX  64

struct http_url {
    bool     https;
    char     host[HTTP_HOST_MAX + 1];
    uint16_t port;
    char     path[HTTP_PATH_MAX + 1];     /* commence par '/' */
};

/* "http://hôte[:port][/chemin]" ou "https://…" (sans casse pour le schéma). */
bool http_url_parse(const char *url, struct http_url *u);

/* Cible d'une redirection : URL absolue, "//hôte/…" ou "/chemin" (relatif à base). */
bool http_url_resolve(const struct http_url *base, const char *location, struct http_url *out);

struct http_resp {
    int  status;                          /* 200, 206, 301…                 */
    long content_length;                  /* -1 si absent                   */
    bool chunked;                         /* Transfer-Encoding: chunked     */
    char location[HTTP_HOST_MAX + HTTP_PATH_MAX + 16];
    char content_type[HTTP_TYPE_MAX + 1];
};

/* En-têtes complets (jusqu'à la ligne vide comprise) ; false si mal formés. */
bool http_resp_parse(const char *hdr, size_t len, struct http_resp *r);

/* Vrai pour 301, 302, 303, 307, 308 avec un Location. */
bool http_is_redirect(const struct http_resp *r);

/* Décodeur « chunked », octet par octet. */
struct http_chunked {
    int           state;
    unsigned long left;                   /* octets restants du morceau     */
    bool          done;                   /* morceau final et trailers lus  */
    bool          error;
};

void http_chunked_init(struct http_chunked *c);
/* Renvoie true si c est un octet du corps (à transmettre), false sinon. */
bool http_chunked_feed(struct http_chunked *c, uint8_t c_byte);

#endif
