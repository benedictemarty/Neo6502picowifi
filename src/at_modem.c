/*
 * at_modem.c — cœur portable du modem Wi-Fi (voir at_modem.h).
 *
 * Formats de réponse calqués sur le firmware AT ESP8266 1.x, tels que les
 * attendent netsetup.pas / netinfo.pas / prophet.pas (séquences cherchées :
 * "+CWMODE:", "STATUS:", "+CWJAP_CUR:", ":ip:", ":gateway:", ":netmask:",
 * "+CIPDNS_CUR:", "+CIFSR:STAIP,", "+CIFSR:STAMAC,", "+CWDHCP_DEF:",
 * "+CWLAP:(", "+CIPSNTPCFG:", "+CIPSNTPTIME:", "+CIPSSLCCONF:", "+IPD,", "OK").
 */
#include "at_modem.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ util */

static void out(struct at_modem *m, const char *s)
{
    m->ops->write(m->ops->ctx, (const uint8_t *)s, strlen(s));
}

static void outf(struct at_modem *m, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out(m, buf);
}

static void ok(struct at_modem *m)    { out(m, "\r\nOK\r\n"); }
static void error(struct at_modem *m) { out(m, "\r\nERROR\r\n"); }

static uint32_t now(struct at_modem *m) { return m->ops->millis(m->ops->ctx); }

static void copy_str(char *dst, size_t dst_size, const char *src, size_t n)
{
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Lit une chaîne entre guillemets à partir de *p ; avance *p. */
static bool parse_quoted(const char **p, char *dst, size_t dst_size)
{
    const char *s = *p;
    while (*s == ' ') s++;
    if (*s != '"') return false;
    s++;
    const char *e = strchr(s, '"');
    if (!e) return false;
    copy_str(dst, dst_size, s, (size_t)(e - s));
    *p = e + 1;
    return true;
}

static bool parse_int(const char **p, long *v)
{
    const char *s = *p;
    while (*s == ' ') s++;
    char *end;
    long r = strtol(s, &end, 10);
    if (end == s) return false;
    *v = r;
    *p = end;
    return true;
}

static bool skip_comma(const char **p)
{
    const char *s = *p;
    while (*s == ' ') s++;
    if (*s != ',') return false;
    *p = s + 1;
    return true;
}

static bool starts(const char *s, const char *prefix, const char **rest)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0) return false;
    if (rest) *rest = s + n;
    return true;
}

/* ---------------------------------------------------------- tampon RX */

size_t at_modem_rx_space(const struct at_modem *m)
{
    size_t used = (m->rx_head - m->rx_tail + AT_RX_RING_SIZE) % AT_RX_RING_SIZE;
    return AT_RX_RING_SIZE - 1 - used;
}

static size_t rx_used(const struct at_modem *m)
{
    return (m->rx_head - m->rx_tail + AT_RX_RING_SIZE) % AT_RX_RING_SIZE;
}

size_t at_modem_rx_push(struct at_modem *m, const uint8_t *data, size_t len)
{
    size_t space = at_modem_rx_space(m);
    if (len > space) return 0;              /* tout ou rien : lwIP réessaiera */
    size_t head = m->rx_head;
    for (size_t i = 0; i < len; i++) {
        m->rx_ring[head] = data[i];
        head = (head + 1) % AT_RX_RING_SIZE;
    }
    m->rx_head = head;
    return len;
}

/* En UDP, chaque datagramme est rangé derrière sa longueur (2 octets, poids
   fort d'abord) ; rx_head n'avance qu'une fois le datagramme complet. */
size_t at_modem_rx_push_dgram(struct at_modem *m, const uint8_t *data, size_t len)
{
    if (len == 0 || len > AT_UDP_MAX || len + 2 > at_modem_rx_space(m)) return 0;
    size_t head = m->rx_head;
    m->rx_ring[head] = (uint8_t)(len >> 8);
    head = (head + 1) % AT_RX_RING_SIZE;
    m->rx_ring[head] = (uint8_t)len;
    head = (head + 1) % AT_RX_RING_SIZE;
    for (size_t i = 0; i < len; i++) {
        m->rx_ring[head] = data[i];
        head = (head + 1) % AT_RX_RING_SIZE;
    }
    m->rx_head = head;
    return len;
}

static void rx_flush(struct at_modem *m) { m->rx_tail = m->rx_head; }

static size_t rx_pop(struct at_modem *m, uint8_t *dst, size_t max)
{
    size_t n = 0, tail = m->rx_tail;
    while (n < max && tail != m->rx_head) {
        dst[n++] = m->rx_ring[tail];
        tail = (tail + 1) % AT_RX_RING_SIZE;
    }
    m->rx_tail = tail;
    return n;
}

void at_modem_remote_closed(struct at_modem *m) { m->remote_closed = true; }

void at_modem_ring(struct at_modem *m) { m->ring_pending = true; }

/* ------------------------------------------------------------- config */

void at_modem_config_defaults(struct at_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->magic = AT_CONFIG_MAGIC;
    cfg->echo = 1;
    cfg->dhcp = 1;
    cfg->sntp_enable = 0;
    cfg->sntp_tz = 0;
    strcpy(cfg->sntp_server, "pool.ntp.org");
    strcpy(cfg->ap_pass, AT_AP_PASS_DEFAULT);
    cfg->tnfs_port = 16384;
    cfg->tnfs_usb = 0;      /* appareil USB identique à la 0.3.x tant qu'on ne l'active pas */
}

static bool has_tls(struct at_modem *m)
{
    return m->ops->tls_info && m->ops->tls_info(m->ops->ctx) != NULL;
}

/* ------------------------------------------------- hôtes autorisés (US-T12) */

bool at_modem_host_pattern_valid(const char *p)
{
    size_t n = strlen(p);
    if (n == 0 || n > AT_HOST_MAX) return false;
    if (p[0] == '*') {
        if (p[1] != '.' || n < 3) return false;
        p += 2;
    }
    for (; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return false;
    return true;
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

bool at_modem_host_allowed(const struct at_config *cfg, const char *host)
{
    if (!cfg->hosts_enforce) return true;
    size_t hl = strlen(host);
    for (int i = 0; i < AT_HOSTS_MAX; i++) {
        const char *pat = cfg->hosts[i];
        if (!pat[0]) continue;
        if (pat[0] == '*') {
            const char *suffix = pat + 1;            /* ".domaine" */
            size_t sl = strlen(suffix);
            if (hl > sl && ieq(host + hl - sl, suffix)) return true;
        } else if (ieq(host, pat)) {
            return true;
        }
    }
    return false;
}

void at_modem_log(struct at_modem *m, const char *kind, const char *host, uint16_t port, bool allowed)
{
    struct at_log_entry *e = &m->log[m->log_count % AT_LOG_MAX];
    e->ms = m->ops->millis ? m->ops->millis(m->ops->ctx) : 0;
    copy_str(e->kind, sizeof e->kind, kind, strlen(kind));
    copy_str(e->host, sizeof e->host, host, strlen(host));
    e->port = port;
    e->allowed = allowed;
    m->log_count++;
}

const struct at_log_entry *at_modem_log_get(const struct at_modem *m, unsigned i)
{
    unsigned n = m->log_count < AT_LOG_MAX ? m->log_count : AT_LOG_MAX;
    if (i >= n) return NULL;
    return &m->log[(m->log_count - 1 - i) % AT_LOG_MAX];
}

/* Contrôle + journal d'une connexion sortante ; false = refusée. */
static bool check_host(struct at_modem *m, const char *kind, const char *host, uint16_t port)
{
    bool ok_ = at_modem_host_allowed(&m->cfg, host);
    at_modem_log(m, kind, host, port, ok_);
    return ok_;
}

bool at_modem_port_is_tls(const struct at_config *cfg, uint16_t port)
{
    for (int i = 0; i < AT_TLS_PORTS_MAX; i++)
        if (cfg->tls_ports[i] && cfg->tls_ports[i] == port) return true;
    return false;
}

/* Phrase de passe WPA2 : 8 à 63 caractères ASCII imprimables. */
static bool ap_pass_valid(const char *pw)
{
    size_t n = 0;
    while (n <= AT_PASS_MAX && pw[n]) n++;     /* borné : champ venu de la flash */
    if (n < 8 || n > 63) return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)pw[i] < 0x20 || (unsigned char)pw[i] > 0x7e) return false;
    return true;
}

static void save(struct at_modem *m)
{
    if (m->ops->config_save) m->ops->config_save(m->ops->ctx, &m->cfg);
}

void at_modem_init(struct at_modem *m, const struct at_modem_ops *ops,
                   const struct at_config *cfg)
{
    memset(m, 0, sizeof *m);
    m->ops = ops;
    if (cfg && cfg->magic == AT_CONFIG_MAGIC) {
        m->cfg = *cfg;
        m->cfg.ap_pass[AT_PASS_MAX] = 0;
        if (!ap_pass_valid(m->cfg.ap_pass)) strcpy(m->cfg.ap_pass, AT_AP_PASS_DEFAULT);
        m->cfg.tnfs_host[AT_HOST_MAX] = 0;
        if (!m->cfg.tnfs_port) m->cfg.tnfs_port = 16384;
        if (m->cfg.tnfs_usb > 1) m->cfg.tnfs_usb = 0;
        /* liste abîmée : entrée invalide effacée ; mode inconnu → filtrage actif
           (en cas de doute, on refuse plutôt que d'ouvrir) */
        for (int i = 0; i < AT_HOSTS_MAX; i++) {
            m->cfg.hosts[i][AT_HOST_MAX] = 0;
            if (m->cfg.hosts[i][0] && !at_modem_host_pattern_valid(m->cfg.hosts[i])) m->cfg.hosts[i][0] = 0;
        }
        if (m->cfg.hosts_enforce > 1) m->cfg.hosts_enforce = 1;
    } else if (cfg && (cfg->magic == AT_CONFIG_MAGIC_V2 || cfg->magic == AT_CONFIG_MAGIC_V1)) {
        /* migration v1/v2 → v3 : les champs ajoutés prennent leur valeur par
           défaut (tls_ports à zéro en v1, ap_pass) ; le reste est conservé */
        m->cfg = *cfg;
        if (cfg->magic == AT_CONFIG_MAGIC_V1) memset(m->cfg.tls_ports, 0, sizeof m->cfg.tls_ports);
        strcpy(m->cfg.ap_pass, AT_AP_PASS_DEFAULT);
        memset(m->cfg.tnfs_host, 0, sizeof m->cfg.tnfs_host);
        m->cfg.tnfs_port = 16384;
        m->cfg.tnfs_usb = 0;
        m->cfg.hosts_enforce = 0;
        memset(m->cfg.hosts, 0, sizeof m->cfg.hosts);
        m->cfg.magic = AT_CONFIG_MAGIC;
    } else {
        at_modem_config_defaults(&m->cfg);
    }
    m->s2 = '+';
    m->s12 = 50;
    m->nfs_wfd = -1;
}

/* --------------------------------------------------------- Wi-Fi/IP */

static int cipstatus(struct at_modem *m)
{
    /* ESP8266 : 2 = IP obtenue, 3 = connexion TCP ouverte, 4 = TCP fermée,
       5 = pas d'AP. netinfo/netsetup : connecté ⇔ 1 < stat < 5. */
    if (!m->ops->wifi_connected(m->ops->ctx)) return 5;
    if (m->ops->tcp_connected(m->ops->ctx)) return 3;
    return m->was_connected ? 4 : 2;
}

static void do_join(struct at_modem *m, const char *ssid, const char *pass, bool persist)
{
    if (persist) {
        strcpy(m->cfg.ssid, ssid);
        strcpy(m->cfg.pass, pass);
        save(m);
    }
    int r = m->ops->wifi_join(m->ops->ctx, ssid, pass);
    if (r == AT_NET_OK) {
        out(m, "WIFI CONNECTED\r\nWIFI GOT IP\r\n");
        ok(m);
    } else {
        int code = (r == AT_NET_TIMEOUT) ? 1 : (r == AT_NET_BAD_PASSWORD) ? 2
                 : (r == AT_NET_NO_AP) ? 3 : 4;
        outf(m, "+CWJAP:%d\r\n\r\nFAIL\r\n", code);
    }
}

static void scan_cb(void *ctx, int ecn, const char *ssid, int rssi)
{
    struct at_modem *m = ctx;
    /* AT+CWLAPOPT=1,7 : ecn, ssid, rssi (masque 7), triés par rssi */
    outf(m, "+CWLAP:(%d,\"%s\",%d)\r\n", ecn, ssid, rssi);
}

/* ------------------------------------------------------------ Hayes */

static void go_online(struct at_modem *m)
{
    m->mode = AT_MODE_ONLINE;
    m->plus_count = 0;
    m->escape_pending = false;
    m->last_rx_ms = now(m);
    out(m, "\r\nCONNECT\r\n");
}

static void hangup(struct at_modem *m)
{
    m->http.active = false;
    if (m->ops->tcp_connected(m->ops->ctx)) m->ops->tcp_close(m->ops->ctx);
    if (m->link_udp) { m->link_udp = false; rx_flush(m); }  /* datagrammes non lus */
    m->remote_closed = false;
    m->mode = AT_MODE_COMMAND;
}

/* ATDT hôte:port ou ATDT hôte port ; port 23 par défaut. */
static void do_dial(struct at_modem *m, const char *arg)
{
    char host[AT_HOST_MAX + 1];
    long port = 23;
    while (*arg == ' ') arg++;
    const char *sep = strpbrk(arg, ": ");
    size_t hl = sep ? (size_t)(sep - arg) : strlen(arg);
    if (hl == 0 || hl > AT_HOST_MAX) { error(m); return; }
    copy_str(host, sizeof host, arg, hl);
    if (sep) {
        const char *p = sep + 1;
        if (!parse_int(&p, &port) || port < 1 || port > 65535) { error(m); return; }
    }
    if (!m->ops->wifi_connected(m->ops->ctx)) { out(m, "\r\nNO CARRIER\r\n"); return; }
    if (m->http.active && !m->ops->tcp_connected(m->ops->ctx)) m->http.active = false;
    if (m->ops->tcp_connected(m->ops->ctx)) { error(m); return; }
    if (!check_host(m, "DIAL", host, (uint16_t)port)) { out(m, "\r\nNO CARRIER\r\n"); return; }
    int r = m->ops->tcp_connect(m->ops->ctx, host, (uint16_t)port,
                                at_modem_port_is_tls(&m->cfg, (uint16_t)port));
    if (r != AT_NET_OK) { out(m, "\r\nNO CARRIER\r\n"); return; }
    m->was_connected = true;
    go_online(m);
}

/* Commandes Hayes classiques : "AT" suivi de lettres. Retourne false si
   la ligne n'est pas une commande Hayes reconnue. */
static bool hayes(struct at_modem *m, const char *cmd)
{
    if (cmd[0] == 0) { ok(m); return true; }               /* AT */
    char c = (char)toupper((unsigned char)cmd[0]);
    const char *arg = cmd + 1;
    long v;
    switch (c) {
    case 'D':
        if (toupper((unsigned char)*arg) == 'T' || toupper((unsigned char)*arg) == 'P') arg++;
        do_dial(m, arg);
        return true;
    case 'H':
        hangup(m);
        ok(m);
        return true;
    case 'O':
        if (m->ops->tcp_connected(m->ops->ctx) && !m->link_udp) go_online(m);
        else out(m, "\r\nNO CARRIER\r\n");
        return true;
    case 'A':
        if (!m->link_udp && !m->cfg.hosts_enforce && m->ops->tcp_accept(m->ops->ctx)) {
            m->ring_pending = false;
            m->ring_count = 0;
            m->was_connected = true;
            go_online(m);
        } else out(m, "\r\nNO CARRIER\r\n");
        return true;
    case 'E':
        m->cfg.echo = (*arg == '1');
        ok(m);
        return true;
    case 'Z':
        hangup(m);
        m->s2 = '+';
        m->s12 = 50;
        ok(m);
        return true;
    case 'I':
        outf(m, "Neo6502drive Pico W modem %s\r\n", m->ops->version(m->ops->ctx));
        if (m->ops->build) outf(m, "build: %s\r\n", m->ops->build(m->ops->ctx));
        if (m->ops->boot_info) outf(m, "%s\r\n", m->ops->boot_info(m->ops->ctx));
        outf(m, "saved SSID: \"%s\"%s\r\n", m->cfg.ssid, m->cfg.echo ? "" : " (echo off)");
        if (m->ops->ap_setup_ssid) {
            const char *ap = m->ops->ap_setup_ssid(m->ops->ctx);
            if (ap) outf(m, "setup AP: \"%s\", password \"%s\", http://192.168.4.1/\r\n", ap, m->cfg.ap_pass);
            else out(m, "setup AP: off\r\n");
        }
        if (m->cfg.hosts_enforce) {
            int n = 0;
            for (int i = 0; i < AT_HOSTS_MAX; i++) n += m->cfg.hosts[i][0] != 0;
            outf(m, "host filter: on, %d host(s) allowed (AT+NHOSTS?, AT+NLOG?)\r\n", n);
        } else {
            out(m, "host filter: off\r\n");
        }
        if (!m->cfg.tnfs_usb) out(m, "TNFS (USB port 2): disabled (AT$TNFSUSB=1)\r\n");
        else if (m->cfg.tnfs_host[0]) outf(m, "TNFS (USB port 2): %s:%u\r\n", m->cfg.tnfs_host, m->cfg.tnfs_port);
        else out(m, "TNFS (USB port 2): no server (AT$TNFS)\r\n");
        if (has_tls(m)) {
            out(m, m->ops->tls_info(m->ops->ctx));   /* peut dépasser le tampon d'outf */
            out(m, "\r\n");
            out(m, "TLS ports:");
            for (int i = 0; i < AT_TLS_PORTS_MAX; i++)
                if (m->cfg.tls_ports[i]) outf(m, " %u", m->cfg.tls_ports[i]);
            out(m, "\r\n");
        }
        ok(m);
        return true;
    case '&':
        /* AT&W : la configuration est déjà enregistrée à chaque commande */
        if (toupper((unsigned char)*arg) == 'W') { ok(m); return true; }
        return false;
    case 'S': {
        const char *p = arg;
        long reg;
        if (!parse_int(&p, &reg)) { error(m); return true; }
        uint8_t *r = (reg == 0) ? &m->cfg.s0 : (reg == 2) ? &m->s2
                   : (reg == 12) ? &m->s12 : NULL;
        if (!r) { error(m); return true; }
        if (*p == '?') { outf(m, "%03d\r\n", *r); ok(m); return true; }
        if (*p == '=' ) {
            p++;
            if (!parse_int(&p, &v) || v < 0 || v > 255) { error(m); return true; }
            *r = (uint8_t)v;
            if (reg == 0) save(m);
            ok(m);
            return true;
        }
        error(m);
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------- AT+ ESP8266 */

/* ------------------------------------------------------ HTTP (US-T11) */

#define HTTP_TIMEOUT_MS  10000
#define HTTP_REDIRECTS   5

static int pop_byte(struct at_modem *m)
{
    uint8_t b;
    return rx_pop(m, &b, 1) ? b : -1;
}

/* En-têtes de réponse gardés pour http_resp_parse ; les autres sont sautés. */
static bool header_wanted(const char *l, size_t n)
{
    static const char *const wanted[] = { "content-length:", "transfer-encoding:", "location:", "content-type:" };
    for (size_t i = 0; i < sizeof wanted / sizeof wanted[0]; i++) {
        size_t k = strlen(wanted[i]), j = 0;
        if (n < k) continue;
        while (j < k && tolower((unsigned char)l[j]) == wanted[i][j]) j++;
        if (j == k) return true;
    }
    return false;
}

/* Messages d'échec de connexion, comme AT+CIPSTART. */
static void connect_error(struct at_modem *m, int r)
{
    if (r == AT_NET_DNS_FAIL) out(m, "DNS Fail\r\n");
    else if (r == AT_NET_NO_TIME) out(m, "no time (SNTP) for TLS\r\n");
    else if (r == AT_NET_TLS_FAIL) out(m, "TLS handshake failed\r\n");
    else if (r == AT_NET_NO_TLS) out(m, "no TLS\r\n");
    error(m);
}

static void http_fail(struct at_modem *m, const char *why)
{
    hangup(m);
    rx_flush(m);
    out(m, why);
    error(m);
}

/* AT+HTTPGET="url"[,début[,fin]] : GET (TLS pour https), redirections
   suivies (5 au plus), en-têtes gardés dans le modem.
   → +HTTPGET:<code>,<taille ou -1>,"<type>" puis OK ; le corps se lit par
   AT+HTTPREAD. Occupe le lien unique (comme CIPSTART). */
static void http_get(struct at_modem *m, const char *p)
{
    static char url[AT_LINE_MAX];
    static char hdr[HTTP_HDR_MAX];          /* statut + en-têtes utiles seulement */
    static char line[HTTP_LINE_MAX];
    static struct http_resp resp;
    long from = -1, to = -1;
    struct http_url u;
    if (!m->ops->idle || !parse_quoted(&p, url, sizeof url)) { error(m); return; }
    if (skip_comma(&p)) {
        if (!parse_int(&p, &from) || from < 0) { error(m); return; }
        if (skip_comma(&p) && (!parse_int(&p, &to) || to < from)) { error(m); return; }
    }
    if (*p) { error(m); return; }
    if (!http_url_parse(url, &u)) { out(m, "bad URL\r\n"); error(m); return; }
    if (m->http.active) hangup(m);                          /* session précédente */
    if (m->ops->tcp_connected(m->ops->ctx)) { out(m, "ALREADY CONNECTED\r\n"); error(m); return; }
    if (!m->ops->wifi_connected(m->ops->ctx)) { out(m, "no ip\r\n"); error(m); return; }

    for (int hop = 0; ; hop++) {
        if (!check_host(m, u.https ? "HTTPS" : "HTTP", u.host, u.port)) {
            out(m, "host not allowed\r\n"); error(m); return;
        }
        rx_flush(m);
        m->remote_closed = false;
        int r = m->ops->tcp_connect(m->ops->ctx, u.host, u.port, u.https);
        if (r != AT_NET_OK) { connect_error(m, r); return; }
        m->was_connected = true;
        m->link_udp = false;

        /* requête : HTTP/1.1, une seule par connexion */
        char *req = (char *)m->send_buf;
        int n = snprintf(req, AT_SEND_MAX, "GET %s HTTP/1.1\r\nHost: %s", u.path, u.host);
        if (u.port != (u.https ? 443 : 80)) n += snprintf(req + n, AT_SEND_MAX - (size_t)n, ":%u", u.port);
        n += snprintf(req + n, AT_SEND_MAX - (size_t)n, "\r\nUser-Agent: Neo6502picowifi/%s\r\n"
                      "Accept-Encoding: identity\r\nConnection: close\r\n", m->ops->version(m->ops->ctx));
        if (from >= 0 && to >= 0) n += snprintf(req + n, AT_SEND_MAX - (size_t)n, "Range: bytes=%ld-%ld\r\n", from, to);
        else if (from >= 0) n += snprintf(req + n, AT_SEND_MAX - (size_t)n, "Range: bytes=%ld-\r\n", from);
        n += snprintf(req + n, AT_SEND_MAX - (size_t)n, "\r\n");
        if (m->ops->tcp_send(m->ops->ctx, m->send_buf, (size_t)n) != AT_NET_OK) {
            http_fail(m, "send failed\r\n"); return;
        }

        /* en-têtes : lus ligne par ligne jusqu'à la ligne vide (la suite reste
           dans le tampon) ; seuls la ligne de statut et les en-têtes utiles sont
           gardés, la taille totale n'est donc pas limitée (github.com : > 4 Ko) */
        size_t hl = 0, ll = 0;
        bool first = true, skipping = false;
        uint32_t t0 = now(m);
        for (;;) {
            int b = pop_byte(m);
            if (b >= 0) {
                if (!skipping) {
                    if (ll == sizeof line) {
                        if (first || header_wanted(line, ll)) { http_fail(m, "HTTP header too large\r\n"); return; }
                        skipping = true;                       /* longue ligne sans intérêt */
                    } else {
                        line[ll++] = (char)b;
                    }
                }
                if (b != '\n') continue;
                bool blank = !skipping && ll <= 2;
                if (!skipping && (first || blank || header_wanted(line, ll))) {
                    if (hl + ll > sizeof hdr) { http_fail(m, "HTTP header too large\r\n"); return; }
                    memcpy(hdr + hl, line, ll);
                    hl += ll;
                }
                first = skipping = false;
                ll = 0;
                if (blank) break;
                continue;
            }
            if (m->remote_closed) { http_fail(m, "connection closed\r\n"); return; }
            if (now(m) - t0 >= HTTP_TIMEOUT_MS) { http_fail(m, "timeout\r\n"); return; }
            m->ops->idle(m->ops->ctx);
        }
        if (!http_resp_parse(hdr, hl, &resp)) { http_fail(m, "bad HTTP response\r\n"); return; }
        struct http_url next;
        if (http_is_redirect(&resp) && hop < HTTP_REDIRECTS && http_url_resolve(&u, resp.location, &next)) {
            hangup(m);
            u = next;
            continue;
        }
        break;
    }
    m->http.active = true;
    m->http.chunked = resp.chunked;
    m->http.remaining = resp.content_length;
    http_chunked_init(&m->http.ch);
    if (resp.status == 204 || resp.status == 304 || (resp.status >= 100 && resp.status < 200)) m->http.remaining = 0;
    if (m->http.chunked) m->http.remaining = -1;
    m->http.eof = m->http.remaining == 0;
    outf(m, "+HTTPGET:%d,%ld,\"%s\"\r\n", resp.status, m->http.remaining, resp.content_type);
    ok(m);
}

/* AT+HTTPREAD=n (1..2048) → +HTTPREAD:<k>,<suite 0|1>: puis k octets du
   corps, puis OK. Attend au plus 10 s le premier octet ; k = 0 avec suite = 1
   si rien n'est encore arrivé ; suite = 0 : corps terminé (lien fermé). */
static void http_read(struct at_modem *m, const char *p)
{
    long n;
    if (!parse_int(&p, &n) || n < 1 || n > AT_SEND_MAX || *p) { error(m); return; }
    if (!m->http.active) { out(m, "no HTTP session\r\n"); error(m); return; }
    uint8_t *buf = m->send_buf;
    size_t k = 0;
    uint32_t t0 = now(m);
    while (k < (size_t)n && !m->http.eof) {
        int b = pop_byte(m);
        if (b < 0) {
            if (m->remote_closed) { m->http.eof = true; break; }   /* fin (ou corps tronqué) */
            if (k > 0 || now(m) - t0 >= HTTP_TIMEOUT_MS) break;
            m->ops->idle(m->ops->ctx);
            continue;
        }
        if (m->http.chunked) {
            if (http_chunked_feed(&m->http.ch, (uint8_t)b)) buf[k++] = (uint8_t)b;
            if (m->http.ch.done || m->http.ch.error) m->http.eof = true;
        } else {
            buf[k++] = (uint8_t)b;
            if (m->http.remaining > 0 && --m->http.remaining == 0) m->http.eof = true;
        }
    }
    outf(m, "+HTTPREAD:%u,%d:", (unsigned)k, m->http.eof ? 0 : 1);
    m->ops->write(m->ops->ctx, buf, k);
    ok(m);
    if (m->http.eof && m->ops->tcp_connected(m->ops->ctx)) {
        m->ops->tcp_close(m->ops->ctx);                    /* session gardée : HTTPREAD → 0,0 */
        rx_flush(m);
        m->remote_closed = false;
    }
}

/* ------------------------------------------- fichiers TNFS (US-T16) */

static int nfs_xfer_adapter(void *ctx, const uint8_t *req, size_t len, uint8_t *resp, size_t cap, uint32_t t)
{
    struct at_modem *m = ctx;
    return m->ops->nfs_xfer(m->ops->ctx, m->nfs_host, m->nfs_port, req, len, resp, cap, t);
}

static void nfs_sleep_adapter(void *ctx, uint32_t ms)
{
    struct at_modem *m = ctx;
    uint32_t t0 = now(m);
    while (now(m) - t0 < ms) m->ops->idle(m->ops->ctx);
}

/* +NERR:<code>,"<nom>" puis ERROR ; codes locaux rendus positifs (256…) */
static void nfs_err(struct at_modem *m, int code)
{
    outf(m, "+NERR:%d,\"%s\"\r\n", code >= 0 ? code : 255 - code, tnfs_strerror(code));
    error(m);
}

static bool nfs_dir_line(void *ctx, const struct tnfs_dirent *e)
{
    struct at_modem *m = ctx;
    char name[TNFS_PATH_MAX + 1];
    size_t i = 0;
    for (; e->name[i] && i < TNFS_PATH_MAX; i++) name[i] = e->name[i] == '"' ? '\'' : e->name[i];
    name[i] = 0;
    outf(m, "+NDIR:\"%.150s\",%lu,%d\r\n", name, (unsigned long)e->size, e->is_dir ? 1 : 0);
    return true;
}

static void nfs_write_done(struct at_modem *m)
{
    size_t w = 0;
    int r = tnfs_write(&m->nfs, (uint8_t)m->nfs_wfd, m->send_buf, m->send_len, &w);
    m->nfs_wfd = -1;
    m->mode = AT_MODE_COMMAND;
    m->line_len = 0;
    if (r != TNFS_OK) { nfs_err(m, r); return; }
    outf(m, "\r\n+NWRITE:%u\r\n", (unsigned)w);
    ok(m);
}

/* Commandes AT+N… ; false si cmd n'en est pas une (suite de plus_command). */
static bool nfs_command(struct at_modem *m, const char *cmd)
{
    const char *p;
    long h, n, v;
    char path[AT_LINE_MAX], path2[AT_LINE_MAX];
    int r;
    struct tnfs_client *c = &m->nfs;
    if (starts(cmd, "NMOUNT=", &p)) {
        /* AT+NMOUNT="hôte"[,port[,"/chemin"[,"user","pass"]]] */
        char host[AT_HOST_MAX + 1], user[33] = "", pass[65] = "";
        long port = 16384;
        strcpy(path, "/");
        if (!m->ops->nfs_xfer || !m->ops->idle || !parse_quoted(&p, host, sizeof host) || !host[0]) { error(m); return true; }
        if (skip_comma(&p)) {
            if (!parse_int(&p, &port) || port < 1 || port > 65535) { error(m); return true; }
            if (skip_comma(&p)) {
                if (!parse_quoted(&p, path, sizeof path)) { error(m); return true; }
                if (skip_comma(&p) && (!parse_quoted(&p, user, sizeof user) || !skip_comma(&p)
                                       || !parse_quoted(&p, pass, sizeof pass))) { error(m); return true; }
            }
        }
        if (*p) { error(m); return true; }
        if (!m->ops->wifi_connected(m->ops->ctx)) { out(m, "no ip\r\n"); error(m); return true; }
        if (!check_host(m, "NFS", host, (uint16_t)port)) { out(m, "host not allowed\r\n"); error(m); return true; }
        if (c->mounted) tnfs_umount(c);
        strcpy(m->nfs_host, host);
        m->nfs_port = (uint16_t)port;
        tnfs_init(c, nfs_xfer_adapter, nfs_sleep_adapter, m);
        r = tnfs_mount(c, path, user, pass);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        outf(m, "+NMOUNT:%u.%u\r\n", c->version >> 8, c->version & 0xff);
        ok(m);
    } else if (!strcmp(cmd, "NUMOUNT")) {
        tnfs_umount(c);
        ok(m);
    } else if (!strcmp(cmd, "NMOUNT?")) {
        if (c->mounted) outf(m, "+NMOUNT:\"%s\",%u\r\n", m->nfs_host, m->nfs_port);
        else out(m, "+NMOUNT:\"\",0\r\n");
        ok(m);
    } else if (starts(cmd, "NOPEN=", &p)) {
        /* mode 0 lecture, 1 écriture (création, troncature), 2 ajout, 3 lecture/écriture */
        static const uint16_t flags[4] = {
            TNFS_O_RDONLY, TNFS_O_WRONLY | TNFS_O_CREAT | TNFS_O_TRUNC,
            TNFS_O_WRONLY | TNFS_O_CREAT | TNFS_O_APPEND, TNFS_O_RDWR | TNFS_O_CREAT };
        long mode = 0;
        if (!parse_quoted(&p, path, sizeof path)) { error(m); return true; }
        if (skip_comma(&p) && (!parse_int(&p, &mode) || mode < 0 || mode > 3)) { error(m); return true; }
        if (*p) { error(m); return true; }
        uint8_t fd;
        r = tnfs_open(c, path, flags[mode], 0644, &fd);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        outf(m, "+NOPEN:%u\r\n", fd);
        ok(m);
    } else if (starts(cmd, "NREAD=", &p)) {
        if (!parse_int(&p, &h) || h < 0 || h > 255 || !skip_comma(&p) || !parse_int(&p, &n)
            || n < 1 || n > TNFS_IO_MAX || *p) { error(m); return true; }
        size_t got = 0;
        r = tnfs_read(c, (uint8_t)h, m->send_buf, (size_t)n, &got);
        if (r != TNFS_OK && r != TNFS_EEOF) { nfs_err(m, r); return true; }
        outf(m, "+NREAD:%u:", (unsigned)got);      /* 0 = fin du fichier */
        m->ops->write(m->ops->ctx, m->send_buf, got);
        ok(m);
    } else if (starts(cmd, "NWRITE=", &p)) {
        if (!parse_int(&p, &h) || h < 0 || h > 255 || !skip_comma(&p) || !parse_int(&p, &n)
            || n < 1 || n > TNFS_IO_MAX || *p) { error(m); return true; }
        if (!c->mounted) { nfs_err(m, TNFS_ERR_MOUNT); return true; }
        m->nfs_wfd = (int)h;
        m->send_expected = (size_t)n;
        m->send_len = 0;
        m->mode = AT_MODE_CIPSEND;
        out(m, "\r\nOK\r\n> ");
    } else if (starts(cmd, "NCLOSE=", &p)) {
        if (!parse_int(&p, &h) || h < 0 || h > 255 || *p) { error(m); return true; }
        r = tnfs_close(c, (uint8_t)h);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        ok(m);
    } else if (starts(cmd, "NSEEK=", &p)) {
        long from = 0;
        if (!parse_int(&p, &h) || h < 0 || h > 255 || !skip_comma(&p) || !parse_int(&p, &v)) { error(m); return true; }
        if (skip_comma(&p) && (!parse_int(&p, &from) || from < 0 || from > 2)) { error(m); return true; }
        if (*p) { error(m); return true; }
        uint32_t pos = 0;
        r = tnfs_lseek(c, (uint8_t)h, (int32_t)v, (uint8_t)from, &pos);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        outf(m, "+NSEEK:%lu\r\n", (unsigned long)pos);
        ok(m);
    } else if (starts(cmd, "NSTAT=", &p)) {
        struct tnfs_stat st;
        if (!parse_quoted(&p, path, sizeof path) || *p) { error(m); return true; }
        r = tnfs_stat(c, path, &st);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        outf(m, "+NSTAT:%lu,%d,%lu\r\n", (unsigned long)st.size, st.is_dir ? 1 : 0, (unsigned long)st.mtime);
        ok(m);
    } else if (starts(cmd, "NDIR=", &p)) {
        if (!parse_quoted(&p, path, sizeof path) || *p) { error(m); return true; }
        r = tnfs_list(c, path, nfs_dir_line, m);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        ok(m);
    } else if (starts(cmd, "NDEL=", &p) || starts(cmd, "NMKDIR=", &p) || starts(cmd, "NRMDIR=", &p)) {
        if (!parse_quoted(&p, path, sizeof path) || *p) { error(m); return true; }
        r = cmd[1] == 'D' ? tnfs_unlink(c, path) : cmd[1] == 'M' ? tnfs_mkdir(c, path) : tnfs_rmdir(c, path);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        ok(m);
    } else if (starts(cmd, "NREN=", &p)) {
        if (!parse_quoted(&p, path, sizeof path) || !skip_comma(&p) || !parse_quoted(&p, path2, sizeof path2) || *p) {
            error(m); return true;
        }
        r = tnfs_rename(c, path, path2);
        if (r != TNFS_OK) { nfs_err(m, r); return true; }
        ok(m);
    } else {
        return false;
    }
    return true;
}

/* US-T12 : avec le filtrage actif, un programme 6502 ne doit pas pouvoir
   détourner un hôte autorisé (autre réseau Wi-Fi, DNS ou passerelle à lui) :
   ces réglages ne changent alors que depuis la page du point d'accès, et le
   mot de passe de celui-ci n'est plus modifiable en AT. */
static bool network_locked(const struct at_modem *m, const char *cmd)
{
    static const char *const locked[] = {
        "CWJAP=", "CWJAP_CUR=", "CWJAP_DEF=", "CWDHCP=", "CWDHCP_CUR=", "CWDHCP_DEF=",
        "CIPSTA=", "CIPSTA_CUR=", "CIPSTA_DEF=", "CIPDNS=", "CIPDNS_CUR=", "CIPDNS_DEF=",
        "APSETUPPWD=",      /* sinon : mot de passe connu d'un complice à portée */
    };
    if (!m->cfg.hosts_enforce) return false;
    for (size_t i = 0; i < sizeof locked / sizeof locked[0]; i++)
        if (starts(cmd, locked[i], NULL)) return true;
    return false;
}

static void plus_command(struct at_modem *m, const char *cmd)
{
    const char *p;
    long v;
    struct at_ip_info info;

    if (network_locked(m, cmd)) {
        out(m, "locked by host filter: use the setup page (AT+APSETUP=1)\r\n");
        error(m);
        return;
    }
    if (!strcmp(cmd, "GMR")) {
        outf(m, "AT version:1.7.4.0(Neo6502drive)\r\nSDK version:%s\r\n"
                "compile time:%s\r\n"
                "Bin version(Pico W):%s\r\n",
             m->ops->version(m->ops->ctx),
             m->ops->build_date ? m->ops->build_date(m->ops->ctx) : "unknown",
             m->ops->version(m->ops->ctx));
        ok(m);
    } else if (!strcmp(cmd, "RST")) {
        ok(m);
        m->ops->reset(m->ops->ctx);
    } else if (!strcmp(cmd, "APSETUP?")) {
        /* US-W6 : point d'accès de configuration (page web) */
        const char *ap = m->ops->ap_setup_ssid ? m->ops->ap_setup_ssid(m->ops->ctx) : NULL;
        if (!m->ops->ap_setup) { error(m); return; }
        if (ap) outf(m, "+APSETUP:1,\"%s\"\r\n", ap); else out(m, "+APSETUP:0\r\n");
        ok(m);
    } else if (starts(cmd, "APSETUP=", &p)) {
        if (!m->ops->ap_setup || !parse_int(&p, &v) || (v != 0 && v != 1)) { error(m); return; }
        if (m->ops->ap_setup(m->ops->ctx, (int)v) == AT_NET_OK) ok(m); else error(m);
    } else if (!strcmp(cmd, "APSETUPPWD?")) {
        outf(m, "+APSETUPPWD:\"%s\"\r\n", m->cfg.ap_pass);
        ok(m);
    } else if (starts(cmd, "APSETUPPWD=", &p)) {
        /* WPA2 : 8 à 63 caractères ; persistant, pris en compte à la prochaine ouverture */
        char pw[AT_PASS_MAX + 1];
        if (!parse_quoted(&p, pw, sizeof pw) || !ap_pass_valid(pw)) { error(m); return; }
        strcpy(m->cfg.ap_pass, pw);
        save(m);
        ok(m);
    } else if (!strcmp(cmd, "BOOTSEL")) {
        if (!m->ops->bootsel) { error(m); return; }
        ok(m);
        m->ops->bootsel(m->ops->ctx);
    } else if (!strcmp(cmd, "RESTORE")) {
        at_modem_config_defaults(&m->cfg);
        save(m);
        ok(m);
        m->ops->reset(m->ops->ctx);
    } else if (!strcmp(cmd, "CWMODE?") || !strcmp(cmd, "CWMODE_CUR?") || !strcmp(cmd, "CWMODE_DEF?")) {
        out(m, "+CWMODE:1\r\n");
        ok(m);
    } else if (starts(cmd, "CWMODE=", &p) || starts(cmd, "CWMODE_CUR=", &p) || starts(cmd, "CWMODE_DEF=", &p)) {
        if (parse_int(&p, &v) && v == 1) ok(m); else error(m); /* station seulement */
    } else if (starts(cmd, "CWJAP=", &p) || starts(cmd, "CWJAP_CUR=", &p) || starts(cmd, "CWJAP_DEF=", &p)) {
        char ssid[AT_SSID_MAX + 1], pass[AT_PASS_MAX + 1] = "";
        if (!parse_quoted(&p, ssid, sizeof ssid)) { error(m); return; }
        if (skip_comma(&p) && !parse_quoted(&p, pass, sizeof pass)) { error(m); return; }
        do_join(m, ssid, pass, !starts(cmd, "CWJAP_CUR=", NULL));
    } else if (!strcmp(cmd, "CWJAP?") || !strcmp(cmd, "CWJAP_CUR?") || !strcmp(cmd, "CWJAP_DEF?")) {
        if (m->ops->wifi_connected(m->ops->ctx)) {
            m->ops->ip_info(m->ops->ctx, &info);
            char tag[16];
            copy_str(tag, sizeof tag, cmd, strlen(cmd) - 1);
            outf(m, "+%s:\"%s\",\"%s\",%d,%d\r\n", tag, info.ssid, info.bssid, info.channel, info.rssi);
        } else {
            out(m, "No AP\r\n");
        }
        ok(m);
    } else if (!strcmp(cmd, "CWQAP")) {
        hangup(m);
        m->ops->wifi_leave(m->ops->ctx);
        ok(m);
        out(m, "WIFI DISCONNECT\r\n");
    } else if (starts(cmd, "CWLAPOPT=", NULL)) {
        ok(m);
    } else if (starts(cmd, "CWLAP", NULL)) {
        if (m->ops->wifi_scan(m->ops->ctx, scan_cb, m) == AT_NET_OK) ok(m); else error(m);
    } else if (!strcmp(cmd, "CWDHCP_DEF?") || !strcmp(cmd, "CWDHCP_CUR?") || !strcmp(cmd, "CWDHCP?")) {
        char tag[16];
        copy_str(tag, sizeof tag, cmd, strlen(cmd) - 1);
        outf(m, "+%s:%d\r\n", tag, m->cfg.dhcp ? 3 : 1); /* bit1 = station */
        ok(m);
    } else if (starts(cmd, "CWDHCP_DEF=", &p) || starts(cmd, "CWDHCP_CUR=", &p) || starts(cmd, "CWDHCP=", &p)) {
        long mode, en;
        if (!parse_int(&p, &mode) || !skip_comma(&p) || !parse_int(&p, &en)) { error(m); return; }
        m->cfg.dhcp = en ? 1 : 0;
        if (!starts(cmd, "CWDHCP_CUR=", NULL)) save(m);
        ok(m);
    } else if (!strcmp(cmd, "CIPSTATUS")) {
        int st = cipstatus(m);
        outf(m, "STATUS:%d\r\n", st);
        if (st == 3) outf(m, "+CIPSTATUS:0,\"%s\",\"0.0.0.0\",0,0\r\n", m->link_udp ? "UDP" : "TCP");
        ok(m);
    } else if (!strcmp(cmd, "CIFSR")) {
        m->ops->ip_info(m->ops->ctx, &info);
        outf(m, "+CIFSR:STAIP,\"%s\"\r\n+CIFSR:STAMAC,\"%s\"\r\n", info.ip, info.mac);
        ok(m);
    } else if (!strcmp(cmd, "CIPSTA?") || !strcmp(cmd, "CIPSTA_CUR?") || !strcmp(cmd, "CIPSTA_DEF?")) {
        char tag[16];
        copy_str(tag, sizeof tag, cmd, strlen(cmd) - 1);
        m->ops->ip_info(m->ops->ctx, &info);
        outf(m, "+%s:ip:\"%s\"\r\n+%s:gateway:\"%s\"\r\n+%s:netmask:\"%s\"\r\n",
             tag, info.ip, tag, info.gateway, tag, info.netmask);
        ok(m);
    } else if (starts(cmd, "CIPSTA=", &p) || starts(cmd, "CIPSTA_CUR=", &p) || starts(cmd, "CIPSTA_DEF=", &p)) {
        char ip[16], gw[16] = "0.0.0.0", mask[16] = "255.255.255.0";
        if (!parse_quoted(&p, ip, sizeof ip)) { error(m); return; }
        if (skip_comma(&p)) {
            if (!parse_quoted(&p, gw, sizeof gw)) { error(m); return; }
            if (skip_comma(&p) && !parse_quoted(&p, mask, sizeof mask)) { error(m); return; }
        }
        strcpy(m->cfg.static_ip, ip);
        strcpy(m->cfg.static_gw, gw);
        strcpy(m->cfg.static_mask, mask);
        m->cfg.dhcp = 0;
        if (!starts(cmd, "CIPSTA_CUR=", NULL)) save(m);
        ok(m);
    } else if (!strcmp(cmd, "CIPDNS_CUR?") || !strcmp(cmd, "CIPDNS_DEF?") || !strcmp(cmd, "CIPDNS?")) {
        m->ops->ip_info(m->ops->ctx, &info);
        char tag[16];
        copy_str(tag, sizeof tag, cmd, strlen(cmd) - 1);
        outf(m, "+%s:%s\r\n", tag, info.dns); /* ESP : "+CIPDNS_CUR:8.8.8.8" */
        ok(m);
    } else if (starts(cmd, "CIPDNS_DEF=", &p) || starts(cmd, "CIPDNS_CUR=", &p) || starts(cmd, "CIPDNS=", &p)) {
        char dns[16] = "";
        if (!parse_int(&p, &v)) { error(m); return; }
        if (v == 1 && (!skip_comma(&p) || !parse_quoted(&p, dns, sizeof dns))) { error(m); return; }
        strcpy(m->cfg.dns, v ? dns : "");
        if (!starts(cmd, "CIPDNS_CUR=", NULL)) save(m);
        ok(m);
    } else if (!strcmp(cmd, "CIPMUX?")) {
        out(m, "+CIPMUX:0\r\n");
        ok(m);
    } else if (starts(cmd, "CIPMUX=", &p)) {
        if (parse_int(&p, &v) && v == 0) ok(m); else error(m);
    } else if (!strcmp(cmd, "CIPMODE?")) {
        out(m, "+CIPMODE:0\r\n");
        ok(m);
    } else if (starts(cmd, "CIPMODE=", &p)) {
        if (parse_int(&p, &v) && v == 0) ok(m); else error(m);
    } else if (!strcmp(cmd, "CIPSSLCCONF?")) {
        /* ESP : 0 aucune, 1 cert client, 2 vérification CA, 3 les deux.
           Ici la CA est toujours vérifiée quand TLS est disponible. */
        outf(m, "+CIPSSLCCONF:%d\r\n", has_tls(m) ? 2 : 0);
        ok(m);
    } else if (starts(cmd, "CIPSSLCCONF=", &p)) {
        if (!parse_int(&p, &v)) { error(m); return; }
        if (v == 0 || (v == 2 && has_tls(m))) ok(m); else error(m); /* pas de cert client */
    } else if (!strcmp(cmd, "TLSTEST")) {
        if (!m->ops->tls_selftest) { error(m); return; }
        out(m, m->ops->tls_selftest(m->ops->ctx));
        out(m, "\r\n");
        ok(m);
    } else if (!strcmp(cmd, "TLSPORT?")) {
        out(m, "+TLSPORT:");
        for (int i = 0, n = 0; i < AT_TLS_PORTS_MAX; i++)
            if (m->cfg.tls_ports[i]) outf(m, "%s%u", n++ ? "," : "", m->cfg.tls_ports[i]);
        out(m, "\r\n");
        ok(m);
    } else if (starts(cmd, "TLSPORT=", &p)) {
        /* AT+TLSPORT=443[,8443…] ; AT+TLSPORT=0 efface. Persistant. */
        uint16_t ports[AT_TLS_PORTS_MAX] = { 0 };
        int n = 0;
        do {
            if (!parse_int(&p, &v) || v < 0 || v > 65535) { error(m); return; }
            if (v && n < AT_TLS_PORTS_MAX) ports[n++] = (uint16_t)v;
            else if (v) { error(m); return; }
        } while (skip_comma(&p));
        memcpy(m->cfg.tls_ports, ports, sizeof ports);
        save(m);
        ok(m);
    } else if (starts(cmd, "CIPSTART=", &p)) {
        char type[8], host[AT_HOST_MAX + 1];
        long port;
        if (!parse_quoted(&p, type, sizeof type) || !skip_comma(&p)
            || !parse_quoted(&p, host, sizeof host) || !skip_comma(&p)
            || !parse_int(&p, &port) || port < 1 || port > 65535) { error(m); return; }
        if (m->http.active && !m->ops->tcp_connected(m->ops->ctx)) m->http.active = false;  /* session HTTP finie */
        bool tls = false, udp = false;
        if (!strcmp(type, "TCP")) tls = at_modem_port_is_tls(&m->cfg, (uint16_t)port);
        else if (!strcmp(type, "SSL")) tls = true;
        else if (!strcmp(type, "UDP") && m->ops->udp_connect) udp = true;
        else { error(m); return; }
        if (m->ops->tcp_connected(m->ops->ctx)) { out(m, "ALREADY CONNECTED\r\n"); error(m); return; }
        if (!m->ops->wifi_connected(m->ops->ctx)) { out(m, "no ip\r\n"); error(m); return; }
        if (!check_host(m, type, host, (uint16_t)port)) { out(m, "host not allowed\r\n"); error(m); return; }
        if (udp) rx_flush(m);   /* aucun lien ouvert : reste éventuel d'un lien TCP */
        int r = udp ? m->ops->udp_connect(m->ops->ctx, host, (uint16_t)port)
                    : m->ops->tcp_connect(m->ops->ctx, host, (uint16_t)port, tls);
        if (r == AT_NET_DNS_FAIL) { out(m, "DNS Fail\r\n"); error(m); return; }
        if (r == AT_NET_NO_TIME) { out(m, "no time (SNTP) for TLS\r\n"); error(m); return; }
        if (r == AT_NET_TLS_FAIL) { out(m, "TLS handshake failed\r\n"); error(m); return; }
        if (r == AT_NET_NO_TLS) { out(m, "no TLS\r\n"); error(m); return; }
        if (r != AT_NET_OK) { error(m); return; }
        m->remote_closed = false;
        m->was_connected = true;
        m->link_udp = udp;
        out(m, "CONNECT\r\n");
        ok(m);
    } else if (starts(cmd, "CIPSEND=", &p)) {
        if (!parse_int(&p, &v) || v < 1 || v > AT_SEND_MAX) { error(m); return; }
        if (!m->ops->tcp_connected(m->ops->ctx)) { out(m, "link is not valid\r\n"); error(m); return; }
        if (m->link_udp && v > AT_UDP_MAX) { error(m); return; }
        m->send_expected = (size_t)v;
        m->send_len = 0;
        m->mode = AT_MODE_CIPSEND;
        out(m, "\r\nOK\r\n> ");
    } else if (!strcmp(cmd, "CIPCLOSE")) {
        if (!m->ops->tcp_connected(m->ops->ctx)) { error(m); return; }
        hangup(m);
        out(m, "CLOSED\r\n");
        ok(m);
    } else if (starts(cmd, "CIPSERVER=", &p)) {
        long en, port = 23;
        if (!parse_int(&p, &en)) { error(m); return; }
        if (skip_comma(&p) && !parse_int(&p, &port)) { error(m); return; }
        if (en && m->cfg.hosts_enforce) { out(m, "incoming calls disabled (host filter)\r\n"); error(m); return; }
        m->cfg.listen_port = en ? (uint16_t)port : 0;
        save(m);
        if (m->ops->tcp_listen(m->ops->ctx, m->cfg.listen_port) == AT_NET_OK) ok(m); else error(m);
    } else if (!strcmp(cmd, "CIPSNTPCFG?")) {
        outf(m, "+CIPSNTPCFG:%d,%d,\"%s\"\r\n", m->cfg.sntp_enable, m->cfg.sntp_tz, m->cfg.sntp_server);
        ok(m);
    } else if (starts(cmd, "CIPSNTPCFG=", &p)) {
        long en, tz = m->cfg.sntp_tz;
        char server[64];
        strcpy(server, m->cfg.sntp_server);
        if (!parse_int(&p, &en)) { error(m); return; }
        if (skip_comma(&p)) {
            if (!parse_int(&p, &tz) || tz < -11 || tz > 13) { error(m); return; }
            if (skip_comma(&p) && !parse_quoted(&p, server, sizeof server)) { error(m); return; }
        }
        if (strcmp(server, m->cfg.sntp_server) && !check_host(m, "SNTP", server, 123)) {
            out(m, "host not allowed\r\n"); error(m); return;
        }
        m->cfg.sntp_enable = en ? 1 : 0;
        m->cfg.sntp_tz = (int8_t)tz;
        strcpy(m->cfg.sntp_server, server);
        save(m);
        ok(m);
    } else if (!strcmp(cmd, "CIPSNTPTIME?")) {
        char t[40] = "";
        if (m->ops->sntp_time) m->ops->sntp_time(m->ops->ctx, t, sizeof t);
        outf(m, "+CIPSNTPTIME:%s\r\n", t[0] ? t : "Thu Jan 01 00:00:00 1970");
        ok(m);
    } else if (starts(cmd, "PING=", &p)) {
        char host[AT_HOST_MAX + 1];
        if (!parse_quoted(&p, host, sizeof host)) { error(m); return; }
        if (!check_host(m, "PING", host, 0)) { out(m, "host not allowed\r\n"); error(m); return; }
        int ms = m->ops->ping ? m->ops->ping(m->ops->ctx, host) : -1;
        if (ms < 0) { out(m, "+timeout\r\n"); error(m); return; }
        outf(m, "+%d\r\n", ms);
        ok(m);
    } else if (starts(cmd, "HTTPGET=", &p)) {
        http_get(m, p);
    } else if (starts(cmd, "HTTPREAD=", &p)) {
        http_read(m, p);
    } else if (!strcmp(cmd, "HTTPCLOSE")) {
        if (m->http.active) hangup(m);
        ok(m);
    } else if (starts(cmd, "N", NULL) && nfs_command(m, cmd)) {
        /* US-T16 : AT+NMOUNT, AT+NOPEN… (traité) */
    } else if (!strcmp(cmd, "NHOSTS?")) {
        /* US-T12 : lecture seule ; modification depuis la page du point d'accès */
        outf(m, "+NHOSTS:%u", m->cfg.hosts_enforce);
        for (int i = 0; i < AT_HOSTS_MAX; i++)
            if (m->cfg.hosts[i][0]) outf(m, ",\"%s\"", m->cfg.hosts[i]);
        out(m, "\r\n");
        ok(m);
    } else if (starts(cmd, "NHOSTS=", NULL)) {
        out(m, "read-only: use the setup page (AT+APSETUP=1, http://192.168.4.1/)\r\n");
        error(m);
    } else if (!strcmp(cmd, "NLOG?")) {
        uint32_t t = now(m);
        const struct at_log_entry *e;
        for (unsigned i = 0; (e = at_modem_log_get(m, i)); i++)
            outf(m, "+NLOG:%lu,\"%s\",\"%s\",%u,%s\r\n", (unsigned long)((t - e->ms) / 1000),
                 e->kind, e->host, e->port, e->allowed ? "allowed" : "refused");
        ok(m);
    } else if (!strcmp(cmd, "CIUPDATE")) {
        out(m, "no OTA on Pico W: flash a new UF2\r\n");
        error(m);
    } else {
        error(m);
    }
}

/* ------------------------------------------------------- ligne AT */

/* AT$TNFSUSB=0|1|? : second port USB TNFS désactivé (défaut) ou présent, au
   prochain démarrage.
   AT$TNFS : serveur du second port USB (TNFS), commun avec PicoWiFiModemUSB.
   AT$TNFS="hôte",port | AT$TNFS=hôte:port | AT$TNFS=hôte (port 16384)
   AT$TNFS=0 (efface) | AT$TNFS? → $TNFS:"hôte",port. Persistant. */
static void dollar_command(struct at_modem *m, const char *cmd)
{
    const char *p;
    char host[AT_HOST_MAX + 1];
    long port = 16384;
    if (!strcmp(cmd, "TNFSUSB?")) {
        outf(m, "$TNFSUSB:%u\r\n", m->cfg.tnfs_usb);
        ok(m);
        return;
    }
    if (starts(cmd, "TNFSUSB=", &p)) {
        /* second port USB : pris en compte au prochain démarrage (AT+RST) */
        long v;
        if (!parse_int(&p, &v) || (v != 0 && v != 1) || *p) { error(m); return; }
        m->cfg.tnfs_usb = (uint8_t)v;
        save(m);
        ok(m);
        return;
    }
    if (!strcmp(cmd, "TNFS?")) {
        if (m->cfg.tnfs_host[0]) outf(m, "$TNFS:\"%s\",%u\r\n", m->cfg.tnfs_host, m->cfg.tnfs_port);
        else out(m, "$TNFS:\"\",0\r\n");
        ok(m);
        return;
    }
    if (!starts(cmd, "TNFS=", &p)) { error(m); return; }
    while (*p == ' ') p++;
    if (!strcmp(p, "0")) {
        m->cfg.tnfs_host[0] = 0;
        m->cfg.tnfs_port = 16384;
        save(m);
        ok(m);
        return;
    }
    if (*p == '"') {
        if (!parse_quoted(&p, host, sizeof host)) { error(m); return; }
        if (skip_comma(&p) && !parse_int(&p, &port)) { error(m); return; }
    } else {
        const char *sep = strchr(p, ':');
        size_t hl = sep ? (size_t)(sep - p) : strlen(p);
        if (hl > AT_HOST_MAX) { error(m); return; }
        copy_str(host, sizeof host, p, hl);
        if (sep) { p = sep + 1; if (!parse_int(&p, &port)) { error(m); return; } }
        else p += hl;
    }
    while (*p == ' ') p++;
    if (!host[0] || strchr(host, ' ') || *p || port < 1 || port > 65535) { error(m); return; }
    if (!check_host(m, "TNFS", host, (uint16_t)port)) { out(m, "host not allowed\r\n"); error(m); return; }
    strcpy(m->cfg.tnfs_host, host);
    m->cfg.tnfs_port = (uint16_t)port;
    save(m);
    ok(m);
}

static void process_line(struct at_modem *m)
{
    char *line = m->line;
    line[m->line_len] = 0;
    if (m->line_len == 0) return;                          /* ligne vide ignorée */
    if (m->cfg.echo) { out(m, line); out(m, "\r\n"); }
    if (toupper((unsigned char)line[0]) != 'A' || toupper((unsigned char)line[1]) != 'T') {
        error(m);
        return;
    }
    const char *cmd = line + 2;
    if (*cmd == '+') {
        char up[AT_LINE_MAX];
        /* nom en majuscules jusqu'à '=' / '?' ; les arguments restent tels quels */
        size_t i = 0;
        const char *s = cmd + 1;
        for (; s[i] && s[i] != '=' && s[i] != '?'; i++) up[i] = (char)toupper((unsigned char)s[i]);
        strcpy(up + i, s + i);
        plus_command(m, up);
    } else if (*cmd == '$') {
        char up[AT_LINE_MAX];
        size_t i = 0;
        const char *s = cmd + 1;
        for (; s[i] && s[i] != '=' && s[i] != '?'; i++) up[i] = (char)toupper((unsigned char)s[i]);
        strcpy(up + i, s + i);
        dollar_command(m, up);
    } else if (!hayes(m, cmd)) {
        error(m);
    }
}

/* ------------------------------------------------------ entrée série */

/* Mode en ligne. Les '+' qui peuvent former "+++" (temps de garde S12 avant,
   intervalles courts entre eux) sont retenus et ne partent vers le distant que
   si la séquence échoue ; at_modem_poll conclut l'échappement après le temps
   de garde qui suit. Les autres octets sont regroupés en un seul envoi TCP. */
static void online_flush(struct at_modem *m, uint8_t *batch, size_t *n)
{
    if (*n && m->ops->tcp_connected(m->ops->ctx)) m->ops->tcp_send(m->ops->ctx, batch, *n);
    *n = 0;
}

static void online_release_plus(struct at_modem *m, uint8_t *batch, size_t *n, size_t cap)
{
    for (int i = 0; i < m->plus_count; i++) {
        if (*n == cap) online_flush(m, batch, n);
        batch[(*n)++] = m->plus_held[i];
    }
    m->plus_count = 0;
    m->escape_pending = false;
}

static void input_online(struct at_modem *m, const uint8_t *data, size_t len)
{
    uint8_t batch[256];
    size_t n = 0;
    uint32_t guard = (uint32_t)m->s12 * 20;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        uint32_t t = now(m);
        bool hold = false;
        if (c == m->s2) {
            if (m->plus_count == 0) hold = (t - m->last_rx_ms >= guard);
            else if (m->plus_count < 3) hold = (t - m->last_rx_ms < guard);
        }
        if (hold) {
            m->plus_held[m->plus_count++] = c;
            if (m->plus_count == 3) { m->escape_pending = true; m->plus_ms = t; }
        } else {
            if (m->plus_count) online_release_plus(m, batch, &n, sizeof batch);
            if (n == sizeof batch) online_flush(m, batch, &n);
            batch[n++] = c;
        }
        m->last_rx_ms = t;
    }
    online_flush(m, batch, &n);
}

void at_modem_input(struct at_modem *m, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        /* "\r\n" : le '\n' qui suit un '\r' de fin de ligne est absorbé même si
           la commande a changé de mode (AT+CIPSEND=n, ATDT). */
        if (m->skip_lf) {
            m->skip_lf = false;
            if (c == '\n') continue;
        }
        switch (m->mode) {
        case AT_MODE_ONLINE: {
            /* le reste du bloc est traité d'un coup (regroupement TCP) */
            size_t j = i;
            while (j < len && m->mode == AT_MODE_ONLINE) j++;
            input_online(m, data + i, j - i);
            i = j - 1;
            break;
        }
        case AT_MODE_CIPSEND:
            m->send_buf[m->send_len++] = c;
            if (m->send_len == m->send_expected && m->nfs_wfd >= 0) {
                nfs_write_done(m);                 /* AT+NWRITE : données reçues */
            } else if (m->send_len == m->send_expected) {
                int r = m->ops->tcp_send(m->ops->ctx, m->send_buf, m->send_len);
                outf(m, "\r\nRecv %u bytes\r\n", (unsigned)m->send_len);
                out(m, r == AT_NET_OK ? "\r\nSEND OK\r\n" : "\r\nSEND FAIL\r\n");
                m->mode = AT_MODE_COMMAND;
                m->line_len = 0;
            }
            break;
        case AT_MODE_COMMAND:
        default:
            if (c == '\r' || c == '\n') {
                m->skip_lf = (c == '\r');
                process_line(m);
                m->line_len = 0;
            } else if (c == 8 || c == 127) {
                if (m->line_len) m->line_len--;
            } else if (m->line_len < AT_LINE_MAX - 1) {
                m->line[m->line_len++] = (char)c;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------ poll */

void at_modem_poll(struct at_modem *m)
{
    uint8_t buf[AT_UDP_MAX];    /* en TCP : blocs de 1460 (segment Ethernet) */

    /* +++ : temps de garde écoulé après le 3e '+' → mode commande (les '+'
       retenus ne sont pas transmis) ; séquence incomplète → on les transmet. */
    if (m->mode == AT_MODE_ONLINE && m->plus_count
        && now(m) - m->last_rx_ms >= (uint32_t)m->s12 * 20) {
        if (m->escape_pending) {
            m->escape_pending = false;
            m->plus_count = 0;
            m->mode = AT_MODE_COMMAND;
            m->line_len = 0;
            ok(m);
        } else {
            uint8_t batch[4];
            size_t n = 0;
            online_release_plus(m, batch, &n, sizeof batch);
            online_flush(m, batch, &n);
        }
    }

    /* données entrantes */
    while (m->link_udp && rx_used(m) >= 2 && m->mode == AT_MODE_COMMAND) {
        uint8_t hdr[2] = { 0, 0 };
        rx_pop(m, hdr, 2);
        size_t n = rx_pop(m, buf, ((size_t)hdr[0] << 8) | hdr[1]);
        outf(m, "\r\n+IPD,%u:", (unsigned)n);
        m->ops->write(m->ops->ctx, buf, n);
    }
    while (!m->link_udp && !m->http.active && rx_used(m) > 0 && m->mode != AT_MODE_CIPSEND) {
        size_t n = rx_pop(m, buf, 1460);
        if (m->mode == AT_MODE_ONLINE) {
            m->ops->write(m->ops->ctx, buf, n);
        } else {
            outf(m, "\r\n+IPD,%u:", (unsigned)n);
            m->ops->write(m->ops->ctx, buf, n);
        }
    }

    /* fermeture distante */
    if (m->remote_closed && !m->http.active && rx_used(m) == 0 && m->mode != AT_MODE_CIPSEND) {
        m->remote_closed = false;
        if (m->mode == AT_MODE_ONLINE) {
            m->mode = AT_MODE_COMMAND;
            m->line_len = 0;
            out(m, "\r\nNO CARRIER\r\n");
        } else {
            out(m, "CLOSED\r\n");
        }
    }

    /* appel entrant */
    if (m->ring_pending && m->mode == AT_MODE_COMMAND) {
        m->ring_pending = false;
        m->ring_count++;
        out(m, "\r\nRING\r\n");
        if (m->cfg.s0 && m->ring_count >= m->cfg.s0 && !m->cfg.hosts_enforce
            && m->ops->tcp_accept(m->ops->ctx)) {
            m->ring_count = 0;
            m->was_connected = true;
            go_online(m);
        }
    }
}
