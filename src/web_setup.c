/* web_setup.c — page web du point d'accès de configuration (voir web_setup.h). */
#include "web_setup.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BODY_MAX 1024          /* corps de POST accepté */

/* ------------------------------------------------------------ sortie */

struct out {
    char  *b;
    size_t cap, n;
    bool   overflow;
};

static void put(struct out *o, const char *s, size_t len)
{
    if (o->n + len > o->cap) { o->overflow = true; return; }
    memcpy(o->b + o->n, s, len);
    o->n += len;
}

static void puts_(struct out *o, const char *s) { put(o, s, strlen(s)); }

static void printf_(struct out *o, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= sizeof tmp) { o->overflow = true; return; }
    put(o, tmp, (size_t)k);
}

/* Texte rendu sûr dans le HTML (contenu et valeurs d'attributs). */
static void put_html(struct out *o, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  puts_(o, "&amp;");  break;
        case '<':  puts_(o, "&lt;");   break;
        case '>':  puts_(o, "&gt;");   break;
        case '"':  puts_(o, "&quot;"); break;
        case '\'': puts_(o, "&#39;");  break;
        default:   put(o, s, 1);       break;
        }
    }
}

/* ---------------------------------------------------------- analyse */

static const char *find(const char *s, size_t len, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= len; i++)
        if (!memcmp(s + i, needle, k)) return s + i;
    return NULL;
}

/* Valeur d'un en-tête (nom sans casse) dans [hdr, end) ; copie dans dst. */
static bool header(const char *hdr, const char *end, const char *name, char *dst, size_t dst_size)
{
    size_t k = strlen(name);
    const char *line = hdr;
    while (line < end) {
        const char *eol = find(line, (size_t)(end - line), "\r\n");
        if (!eol) eol = end;
        if ((size_t)(eol - line) > k && line[k] == ':') {
            size_t i = 0;
            while (i < k && tolower((unsigned char)line[i]) == tolower((unsigned char)name[i])) i++;
            if (i == k) {
                const char *v = line + k + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t n = (size_t)(eol - v);
                while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
                if (n >= dst_size) n = dst_size - 1;
                memcpy(dst, v, n);
                dst[n] = 0;
                return true;
            }
        }
        line = eol + 2;
    }
    return false;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Champ name d'un corps application/x-www-form-urlencoded, décodé dans dst
   (toujours terminé). Renvoie 1 trouvé, 0 absent (dst vide), -1 trop long. */
static int form_field(const char *body, size_t len, const char *name, char *dst, size_t dst_size)
{
    size_t k = strlen(name);
    const char *p = body, *end = body + len;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        if (!amp) amp = end;
        if ((size_t)(amp - p) > k && !memcmp(p, name, k) && p[k] == '=') {
            size_t n = 0;
            for (const char *s = p + k + 1; s < amp; s++) {
                char c = *s;
                if (c == '+') c = ' ';
                else if (c == '%' && amp - s >= 3 && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
                    c = (char)(hexval(s[1]) * 16 + hexval(s[2]));
                    s += 2;
                }
                if (n + 1 >= dst_size) { dst[0] = 0; return -1; }
                dst[n++] = c;
            }
            dst[n] = 0;
            return 1;
        }
        p = amp + 1;
    }
    dst[0] = 0;
    return 0;
}

/* ---------------------------------------------------------- réponses */

static void redirect(const struct web_status *st, struct out *o)
{
    printf_(o, "HTTP/1.1 302 Found\r\nLocation: http://%s/\r\n"
               "Content-Length: 0\r\nConnection: close\r\n\r\n", st->ap_ip);
}

static void page_body(const struct web_status *st, const char *error, struct out *o)
{
    bool refresh = st->join == WEB_JOIN_RUNNING || st->scanning;
    puts_(o, "<!doctype html><html lang=fr><head><meta charset=utf-8>"
             "<meta name=viewport content=\"width=device-width,initial-scale=1\">");
    if (refresh) puts_(o, "<meta http-equiv=refresh content=3>");
    puts_(o, "<title>Modem Wi-Fi Neo6502</title><style>"
             "body{font-family:sans-serif;max-width:30em;margin:1em auto;padding:0 1em}"
             "label{display:block;margin:.5em 0}"
             "input[type=text],input[type=password]{width:100%;padding:.4em;box-sizing:border-box}"
             "button{padding:.5em 1em;margin-top:.6em}.e{color:#b00}.o{color:#070}"
             "</style></head><body><h1>Modem Wi-Fi Neo6502</h1><p>R\xc3\xa9seau m\xc3\xa9moris\xc3\xa9 : ");
    if (st->saved_ssid[0]) { puts_(o, "<b>"); put_html(o, st->saved_ssid); puts_(o, "</b>"); }
    else puts_(o, "aucun");
    puts_(o, "</p>");
    if (st->sta_ip[0]) printf_(o, "<p class=o>Connect\xc3\xa9, adresse IP %s</p>", st->sta_ip);

    const char *js = st->join_ssid ? st->join_ssid : "";
    switch (st->join) {
    case WEB_JOIN_RUNNING:
        puts_(o, "<p>Connexion \xc3\xa0 \xc2\xab\xc2\xa0"); put_html(o, js);
        puts_(o, "\xc2\xa0\xc2\xbb en cours\xe2\x80\xa6</p>");
        break;
    case WEB_JOIN_OK:
        puts_(o, "<p class=o>Connect\xc3\xa9 \xc3\xa0 \xc2\xab\xc2\xa0"); put_html(o, js);
        puts_(o, "\xc2\xa0\xc2\xbb. Le point d'acc\xc3\xa8s de configuration va se fermer.</p>");
        break;
    case WEB_JOIN_BAD_PASSWORD:
        puts_(o, "<p class=e>Mot de passe refus\xc3\xa9 par \xc2\xab\xc2\xa0"); put_html(o, js);
        puts_(o, "\xc2\xa0\xc2\xbb.</p>");
        break;
    case WEB_JOIN_NO_AP:
        puts_(o, "<p class=e>R\xc3\xa9seau \xc2\xab\xc2\xa0"); put_html(o, js);
        puts_(o, "\xc2\xa0\xc2\xbb introuvable.</p>");
        break;
    case WEB_JOIN_FAIL:
        puts_(o, "<p class=e>\xc3\x89" "chec de la connexion \xc3\xa0 \xc2\xab\xc2\xa0"); put_html(o, js);
        puts_(o, "\xc2\xa0\xc2\xbb.</p>");
        break;
    default:
        break;
    }
    if (error) { puts_(o, "<p class=e>"); put_html(o, error); puts_(o, "</p>"); }

    puts_(o, "<form method=post action=/save><h2>R\xc3\xa9seaux trouv\xc3\xa9s</h2>");
    if (st->scanning) puts_(o, "<p>Recherche en cours\xe2\x80\xa6</p>");
    else if (st->n_ap == 0) puts_(o, "<p>Aucun r\xc3\xa9seau trouv\xc3\xa9.</p>");
    for (int i = 0; i < st->n_ap; i++) {
        /* un SSID échappé tient en 192 octets, l'entrée en ~450 : on garde
           de quoi finir la page plutôt que de tomber en erreur 500 */
        if (o->cap - o->n < 1500) { puts_(o, "<p>\xe2\x80\xa6</p>"); break; }
        puts_(o, "<label><input type=radio name=ssid value=\"");
        put_html(o, st->ap[i].ssid);
        puts_(o, "\"> ");
        put_html(o, st->ap[i].ssid);
        printf_(o, " <small>(%d dBm%s)</small></label>", st->ap[i].rssi, st->ap[i].ecn ? "" : ", ouvert");
    }
    puts_(o, "<p><a href=/scan>Relancer la recherche</a></p>"
             "<label>Autre r\xc3\xa9seau (nom) : <input type=text name=ssid_manual maxlength=32></label>"
             "<label>Mot de passe : <input type=password name=pass maxlength=63></label>"
             "<button type=submit>Enregistrer et se connecter</button></form>");
    printf_(o, "<p><small>Modem %s</small></p></body></html>", st->version);
}

/* Page complète : le corps est construit derrière une réserve, puis l'en-tête
   (qui contient sa longueur) est écrit devant. */
static void page(const struct web_status *st, const char *status_line, const char *error,
                 struct out *o)
{
    enum { RESERVE = 192 };
    if (o->cap < RESERVE) { o->overflow = true; return; }
    struct out body = { o->b + RESERVE, o->cap - RESERVE, 0, false };
    page_body(st, error, &body);
    if (body.overflow) { o->overflow = true; return; }
    char hdr[RESERVE];
    int k = snprintf(hdr, sizeof hdr, "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Cache-Control: no-store\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                     status_line, (unsigned)body.n);
    memmove(o->b + k, body.b, body.n);
    memcpy(o->b, hdr, (size_t)k);
    o->n = (size_t)k + body.n;
}

static void simple(const char *status_line, struct out *o)
{
    printf_(o, "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status_line);
}

/* ----------------------------------------------------------- entrée */

enum web_result web_setup_handle(const char *req, size_t len, const struct web_status *st,
                                 struct web_form *form, char *resp, size_t cap,
                                 size_t *resp_len)
{
    struct out o = { resp, cap, 0, false };
    enum web_result r = WEB_REPLY;
    const char *hend = find(req, len, "\r\n\r\n");
    if (!hend) return WEB_INCOMPLETE;
    const char *eol = find(req, (size_t)(hend - req) + 2, "\r\n");

    /* ligne de requête : méthode, chemin (sans la partie ?…) */
    const char *sp = memchr(req, ' ', (size_t)(eol - req));
    const char *path = sp ? sp + 1 : eol;
    const char *pend = path;
    while (pend < eol && *pend != ' ' && *pend != '?') pend++;
    size_t mlen = sp ? (size_t)(sp - req) : 0, plen = (size_t)(pend - path);

    char host[64] = "";
    header(eol + 2, hend + 2, "Host", host, sizeof host);
    char *colon = strchr(host, ':');
    if (colon && !strcmp(colon, ":80")) *colon = 0;
    bool ours = !host[0] || !strcmp(host, st->ap_ip);

#define IS(m, p) (mlen == strlen(m) && !memcmp(req, m, mlen) && plen == strlen(p) && !memcmp(path, p, plen))
    if (!ours) {
        redirect(st, &o);
    } else if (IS("GET", "/")) {
        page(st, "200 OK", NULL, &o);
    } else if (IS("GET", "/scan")) {
        redirect(st, &o);
        r = WEB_REPLY_RESCAN;
    } else if (IS("POST", "/save")) {
        char cl[16] = "";
        long n = header(eol + 2, hend + 2, "Content-Length", cl, sizeof cl) ? strtol(cl, NULL, 10) : -1;
        const char *body = hend + 4;
        size_t have = len - (size_t)(body - req);
        if (n < 0 || n > BODY_MAX) {
            simple(n < 0 ? "411 Length Required" : "413 Payload Too Large", &o);
        } else if (have < (size_t)n) {
            return WEB_INCOMPLETE;
        } else {
            char manual[34] = "", choice[34] = "", pass[66] = "";
            const char *error = NULL;
            bool ok_manual = form_field(body, (size_t)n, "ssid_manual", manual, sizeof manual) >= 0;
            bool ok_choice = form_field(body, (size_t)n, "ssid", choice, sizeof choice) >= 0;
            bool ok_pass = form_field(body, (size_t)n, "pass", pass, sizeof pass) >= 0;
            const char *ssid = manual[0] ? manual : choice;
            size_t pl = strlen(pass);
            if (!ok_manual || !ok_choice || strlen(ssid) > 32) error = "Nom de r\xc3\xa9seau trop long (32 octets au plus).";
            else if (!ssid[0]) error = "Choisissez un r\xc3\xa9seau ou saisissez son nom.";
            else if (!ok_pass || pl > 63 || (pl > 0 && pl < 8))
                error = "Le mot de passe doit faire de 8 \xc3\xa0 63 caract\xc3\xa8res (vide pour un r\xc3\xa9seau ouvert).";
            if (error) {
                page(st, "400 Bad Request", error, &o);
            } else {
                strcpy(form->ssid, ssid);
                strcpy(form->pass, pass);
                struct web_status next = *st;
                next.join = WEB_JOIN_RUNNING;
                next.join_ssid = form->ssid;
                page(&next, "200 OK", NULL, &o);
                r = WEB_REPLY_SUBMIT;
            }
        }
    } else {
        redirect(st, &o);
    }
#undef IS
    if (o.overflow) {
        o.n = 0;
        o.overflow = false;
        simple("500 Internal Server Error", &o);
        if (r == WEB_REPLY_RESCAN) r = WEB_REPLY;
    }
    *resp_len = o.n;
    return r;
}
