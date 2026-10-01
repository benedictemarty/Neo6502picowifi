/* http_parse.c — voir http_parse.h. */
#include "http_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool prefix_nocase(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false;
    return true;
}

/* hôte[:port][/chemin] après le schéma */
static bool parse_authority(const char *s, struct http_url *u)
{
    const char *end = s + strcspn(s, "/?#");
    const char *colon = memchr(s, ':', (size_t)(end - s));
    const char *hend = colon ? colon : end;
    size_t hl = (size_t)(hend - s);
    if (hl == 0 || hl > HTTP_HOST_MAX) return false;
    for (const char *c = s; c < hend; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-') return false;
    memcpy(u->host, s, hl);
    u->host[hl] = 0;
    u->port = u->https ? 443 : 80;
    if (colon) {
        char *pe;
        long p = strtol(colon + 1, &pe, 10);
        if (pe != end || p < 1 || p > 65535) return false;
        u->port = (uint16_t)p;
    }
    const char *path = *end ? end : "/";
    if (*path != '/') {                    /* "?x" sans chemin : "/?x" */
        if (strlen(path) + 1 > HTTP_PATH_MAX) return false;
        u->path[0] = '/';
        strcpy(u->path + 1, path);
    } else {
        if (strlen(path) > HTTP_PATH_MAX) return false;
        strcpy(u->path, path);
    }
    char *frag = strchr(u->path, '#');     /* le fragment ne part pas au serveur */
    if (frag) *frag = 0;
    for (const char *c = u->path; *c; c++)
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7f) return false;
    return true;
}

bool http_url_parse(const char *url, struct http_url *u)
{
    memset(u, 0, sizeof *u);
    if (prefix_nocase(url, "https://")) { u->https = true; return parse_authority(url + 8, u); }
    if (prefix_nocase(url, "http://")) return parse_authority(url + 7, u);
    return false;
}

bool http_url_resolve(const struct http_url *base, const char *loc, struct http_url *out)
{
    if (prefix_nocase(loc, "http://") || prefix_nocase(loc, "https://")) return http_url_parse(loc, out);
    memset(out, 0, sizeof *out);
    out->https = base->https;
    if (loc[0] == '/' && loc[1] == '/') return parse_authority(loc + 2, out);
    if (loc[0] != '/') return false;       /* chemins relatifs simples non pris en charge */
    strcpy(out->host, base->host);
    out->port = base->port;
    if (strlen(loc) > HTTP_PATH_MAX) return false;
    strcpy(out->path, loc);
    char *frag = strchr(out->path, '#');
    if (frag) *frag = 0;
    for (const char *c = out->path; *c; c++)
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7f) return false;
    return true;
}

/* Valeur d'en-tête, espaces retirés, copiée dans dst (tronquée). */
static void copy_value(const char *v, const char *eol, char *dst, size_t size)
{
    while (v < eol && (*v == ' ' || *v == '\t')) v++;
    size_t n = (size_t)(eol - v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
    if (n >= size) n = size - 1;
    memcpy(dst, v, n);
    dst[n] = 0;
}

bool http_resp_parse(const char *hdr, size_t len, struct http_resp *r)
{
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    const char *end = hdr + len;
    const char *eol = NULL;
    for (const char *p = hdr; p + 1 < end; p++) if (p[0] == '\r' && p[1] == '\n') { eol = p; break; }
    if (!eol || len < 12 || !prefix_nocase(hdr, "HTTP/1.")) return false;
    if (hdr[8] != ' ' || !isdigit((unsigned char)hdr[9]) || !isdigit((unsigned char)hdr[10])
        || !isdigit((unsigned char)hdr[11])) return false;
    r->status = (hdr[9] - '0') * 100 + (hdr[10] - '0') * 10 + (hdr[11] - '0');
    for (const char *line = eol + 2; line < end; ) {
        const char *le = NULL;
        for (const char *p = line; p + 1 < end; p++) if (p[0] == '\r' && p[1] == '\n') { le = p; break; }
        if (!le || le == line) break;      /* ligne vide : fin des en-têtes */
        const char *colon = memchr(line, ':', (size_t)(le - line));
        if (colon) {
            const char *v = colon + 1;
            char tmp[24];
            if (prefix_nocase(line, "Content-Length:")) {
                copy_value(v, le, tmp, sizeof tmp);
                char *pe;
                long n = strtol(tmp, &pe, 10);
                if (*pe || n < 0 || pe == tmp) return false;
                r->content_length = n;
            } else if (prefix_nocase(line, "Transfer-Encoding:")) {
                copy_value(v, le, tmp, sizeof tmp);
                size_t tl = strlen(tmp);
                r->chunked = tl >= 7 && prefix_nocase(tmp + tl - 7, "chunked");
            } else if (prefix_nocase(line, "Location:")) {
                copy_value(v, le, r->location, sizeof r->location);
            } else if (prefix_nocase(line, "Content-Type:")) {
                copy_value(v, le, r->content_type, sizeof r->content_type);
                for (char *c = r->content_type; *c; c++) if (*c == '"') *c = '\'';
            }
        }
        line = le + 2;
    }
    if (r->chunked) r->content_length = -1;   /* RFC 9112 : chunked l'emporte */
    return true;
}

bool http_is_redirect(const struct http_resp *r)
{
    return (r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307
            || r->status == 308) && r->location[0];
}

enum { CH_SIZE, CH_EXT, CH_SIZE_LF, CH_DATA, CH_DATA_CR, CH_DATA_LF, CH_TRAILER, CH_TRAILER_LINE, CH_DONE };

void http_chunked_init(struct http_chunked *c) { memset(c, 0, sizeof *c); }

bool http_chunked_feed(struct http_chunked *c, uint8_t b)
{
    switch (c->state) {
    case CH_SIZE:
        if (isxdigit(b)) {
            if (c->left > 0x0fffffffUL) { c->error = true; c->state = CH_DONE; return false; }
            c->left = c->left * 16 + (unsigned long)(isdigit(b) ? b - '0' : tolower(b) - 'a' + 10);
        } else if (b == ';' || b == ' ' || b == '\t') {
            c->state = CH_EXT;
        } else if (b == '\r') {
            c->state = CH_SIZE_LF;
        } else {
            c->error = true; c->state = CH_DONE;
        }
        return false;
    case CH_EXT:
        if (b == '\r') c->state = CH_SIZE_LF;
        return false;
    case CH_SIZE_LF:
        if (b != '\n') { c->error = true; c->state = CH_DONE; return false; }
        c->state = c->left ? CH_DATA : CH_TRAILER;
        return false;
    case CH_DATA:
        if (--c->left == 0) c->state = CH_DATA_CR;
        return true;
    case CH_DATA_CR:
        if (b != '\r') { c->error = true; c->state = CH_DONE; return false; }
        c->state = CH_DATA_LF;
        return false;
    case CH_DATA_LF:
        if (b != '\n') { c->error = true; c->state = CH_DONE; return false; }
        c->state = CH_SIZE;
        c->left = 0;
        return false;
    case CH_TRAILER:                       /* début de ligne après le morceau final */
        if (b == '\r') return false;
        if (b == '\n') { c->done = true; c->state = CH_DONE; return false; }
        c->state = CH_TRAILER_LINE;
        return false;
    case CH_TRAILER_LINE:
        if (b == '\n') c->state = CH_TRAILER;
        return false;
    default:
        return false;
    }
}
