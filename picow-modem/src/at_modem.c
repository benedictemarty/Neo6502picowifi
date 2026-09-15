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
    if (cfg && cfg->magic == AT_CONFIG_MAGIC) m->cfg = *cfg;
    else at_modem_config_defaults(&m->cfg);
    m->s2 = '+';
    m->s12 = 50;
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
    if (m->ops->tcp_connected(m->ops->ctx)) m->ops->tcp_close(m->ops->ctx);
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
    if (m->ops->tcp_connected(m->ops->ctx)) { error(m); return; }
    int r = m->ops->tcp_connect(m->ops->ctx, host, (uint16_t)port);
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
        if (m->ops->tcp_connected(m->ops->ctx)) go_online(m);
        else out(m, "\r\nNO CARRIER\r\n");
        return true;
    case 'A':
        if (m->ops->tcp_accept(m->ops->ctx)) {
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
        ok(m);
        return true;
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

static void plus_command(struct at_modem *m, const char *cmd)
{
    const char *p;
    long v;
    struct at_ip_info info;

    if (!strcmp(cmd, "GMR")) {
        outf(m, "AT version:1.7.4.0(Neo6502drive)\r\nSDK version:%s\r\n"
                "compile time:" __DATE__ " " __TIME__ "\r\n"
                "Bin version(Pico W):%s\r\n",
             m->ops->version(m->ops->ctx), m->ops->version(m->ops->ctx));
        ok(m);
    } else if (!strcmp(cmd, "RST")) {
        ok(m);
        m->ops->reset(m->ops->ctx);
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
            outf(m, "+%s:\"%s\",\"%s\",%d,%d\r\n", tag, info.ssid, info.mac, info.channel, info.rssi);
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
        if (st == 3) out(m, "+CIPSTATUS:0,\"TCP\",\"0.0.0.0\",0,0\r\n");
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
        out(m, "+CIPSSLCCONF:0\r\n");
        ok(m);
    } else if (starts(cmd, "CIPSSLCCONF=", &p)) {
        if (parse_int(&p, &v) && v == 0) ok(m); else error(m); /* pas de TLS */
    } else if (starts(cmd, "CIPSTART=", &p)) {
        char type[8], host[AT_HOST_MAX + 1];
        long port;
        if (!parse_quoted(&p, type, sizeof type) || !skip_comma(&p)
            || !parse_quoted(&p, host, sizeof host) || !skip_comma(&p)
            || !parse_int(&p, &port) || port < 1 || port > 65535) { error(m); return; }
        if (strcmp(type, "TCP") != 0) { error(m); return; }
        if (m->ops->tcp_connected(m->ops->ctx)) { out(m, "ALREADY CONNECTED\r\n"); error(m); return; }
        if (!m->ops->wifi_connected(m->ops->ctx)) { out(m, "no ip\r\n"); error(m); return; }
        int r = m->ops->tcp_connect(m->ops->ctx, host, (uint16_t)port);
        if (r == AT_NET_DNS_FAIL) { out(m, "DNS Fail\r\n"); error(m); return; }
        if (r != AT_NET_OK) { error(m); return; }
        m->remote_closed = false;
        m->was_connected = true;
        out(m, "CONNECT\r\n");
        ok(m);
    } else if (starts(cmd, "CIPSEND=", &p)) {
        if (!parse_int(&p, &v) || v < 1 || v > AT_SEND_MAX) { error(m); return; }
        if (!m->ops->tcp_connected(m->ops->ctx)) { out(m, "link is not valid\r\n"); error(m); return; }
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
        int ms = m->ops->ping ? m->ops->ping(m->ops->ctx, host) : -1;
        if (ms < 0) { out(m, "+timeout\r\n"); error(m); return; }
        outf(m, "+%d\r\n", ms);
        ok(m);
    } else if (!strcmp(cmd, "CIUPDATE")) {
        out(m, "no OTA on Pico W: flash a new UF2\r\n");
        error(m);
    } else {
        error(m);
    }
}

/* ------------------------------------------------------- ligne AT */

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
    } else if (!hayes(m, cmd)) {
        error(m);
    }
}

/* ------------------------------------------------------ entrée série */

static void input_online(struct at_modem *m, uint8_t c)
{
    uint32_t t = now(m);
    /* Détection de "+++" avec temps de garde avant et après (S12/50 s). */
    uint32_t guard = (uint32_t)m->s12 * 20;
    if (c == m->s2) {
        if (m->plus_count == 0) {
            m->plus_count = (t - m->last_rx_ms >= guard) ? 1 : 0;
        } else if (m->plus_count < 3 && t - m->last_rx_ms < guard) {
            m->plus_count++;
        } else {
            m->plus_count = 0;
        }
        if (m->plus_count == 3) { m->escape_pending = true; m->plus_ms = t; }
    } else {
        m->plus_count = 0;
        m->escape_pending = false;
    }
    m->last_rx_ms = t;
    if (m->ops->tcp_connected(m->ops->ctx)) m->ops->tcp_send(m->ops->ctx, &c, 1);
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
        case AT_MODE_ONLINE:
            input_online(m, c);
            break;
        case AT_MODE_CIPSEND:
            m->send_buf[m->send_len++] = c;
            if (m->send_len == m->send_expected) {
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
    uint8_t buf[1460];

    /* +++ : temps de garde écoulé après le 3e '+' */
    if (m->mode == AT_MODE_ONLINE && m->escape_pending
        && now(m) - m->plus_ms >= (uint32_t)m->s12 * 20) {
        m->escape_pending = false;
        m->plus_count = 0;
        m->mode = AT_MODE_COMMAND;
        m->line_len = 0;
        ok(m);
    }

    /* données entrantes */
    while (rx_used(m) > 0 && m->mode != AT_MODE_CIPSEND) {
        size_t n = rx_pop(m, buf, sizeof buf);
        if (m->mode == AT_MODE_ONLINE) {
            m->ops->write(m->ops->ctx, buf, n);
        } else {
            outf(m, "\r\n+IPD,%u:", (unsigned)n);
            m->ops->write(m->ops->ctx, buf, n);
        }
    }

    /* fermeture distante */
    if (m->remote_closed && rx_used(m) == 0 && m->mode != AT_MODE_CIPSEND) {
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
        if (m->cfg.s0 && m->ring_count >= m->cfg.s0 && m->ops->tcp_accept(m->ops->ctx)) {
            m->ring_count = 0;
            m->was_connected = true;
            go_online(m);
        }
    }
}
