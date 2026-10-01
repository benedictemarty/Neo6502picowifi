/*
 * ap_pico.c — point d'accès de configuration sur Pico W (US-W6).
 *
 * SSID « Neo6502-modem-XXXX » (fin de l'adresse MAC), WPA2, mot de passe
 * AT+APSETUPPWD (défaut AT_AP_PASS_DEFAULT). Sur l'interface du point
 * d'accès seulement : DHCP (dhcp_server.c), DNS captif (dns_catchall.c),
 * page web sur 192.168.4.1:80 (web_setup.c). La station reste active : le
 * réseau choisi est rejoint par la reconnexion de fond de net_pico.c.
 *
 * Les rappels lwIP s'exécutent en interruption ; l'enregistrement en flash,
 * la recherche des réseaux et l'association sont faits dans ap_pico_poll
 * (boucle principale).
 */
#include "ap_pico.h"
#include "net_pico.h"
#include "dhcp_server.h"
#include "dns_catchall.h"
#include "web_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

#define HTTP_CONNS    4
#define HTTP_REQ_MAX  1536
#define HTTP_RESP_MAX 6144

static bool active;
static char ap_ssid[33];
static struct udp_pcb *dhcp_pcb, *dns_pcb;
static struct tcp_pcb *http_pcb;
static struct dhcps dhcps;
static char ap_ip_str[16];

static volatile uint32_t last_http_ms;
static uint32_t ok_ms;                  /* association réussie (fermeture différée) */
static bool auto_checked;               /* règle des 60 s déjà appliquée            */

/* état partagé avec les rappels lwIP (modifié sous cyw43_arch_lwip_begin) */
static struct web_ap aps[WEB_AP_MAX];
static int n_ap;
static volatile bool scanning, scan_pending, form_pending, hosts_pending;
static struct web_form form;
static struct web_hosts hosts_form;     /* US-T12 : liste reçue de la page */
static enum web_join join_state;
static char join_ssid[33];
static uint32_t join_t0;
static bool join_nonet;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* ----------------------------------------------------------------- HTTP */

struct http_conn {
    struct tcp_pcb *pcb;
    char   req[HTTP_REQ_MAX];
    size_t req_len;
    char  *resp;
    size_t resp_len, resp_off;
};
static struct http_conn conns[HTTP_CONNS];

static void conn_free(struct http_conn *c)
{
    free(c->resp);
    memset(c, 0, sizeof *c);
}

static void conn_close(struct http_conn *c)
{
    struct tcp_pcb *p = c->pcb;
    if (p) {
        tcp_arg(p, NULL); tcp_recv(p, NULL); tcp_sent(p, NULL); tcp_err(p, NULL); tcp_poll(p, NULL, 0);
        if (tcp_close(p) != ERR_OK) tcp_abort(p);
    }
    conn_free(c);
}

static void send_more(struct http_conn *c)
{
    while (c->resp_off < c->resp_len) {
        size_t n = tcp_sndbuf(c->pcb);
        if (n > c->resp_len - c->resp_off) n = c->resp_len - c->resp_off;
        if (n == 0) break;
        if (tcp_write(c->pcb, c->resp + c->resp_off, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        c->resp_off += n;
    }
    tcp_output(c->pcb);
    if (c->resp_off == c->resp_len) conn_close(c);   /* tcp_close envoie ce qui reste */
}

static void status_snapshot(struct web_status *st, char *sta_ip, size_t sta_ip_size)
{
    struct at_modem *m = net_pico_modem();
    sta_ip[0] = 0;
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
        ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), sta_ip, (int)sta_ip_size);
    st->ap_ip = ap_ip_str;
    st->saved_ssid = m->cfg.ssid;
    st->sta_ip = sta_ip;
    st->version = net_pico_ops.version(NULL);
    st->join = join_state;
    st->join_ssid = join_ssid;
    st->scanning = scanning || scan_pending;
    st->n_ap = n_ap;
    st->ap = aps;
    st->hosts_enforce = m->cfg.hosts_enforce;
    st->hosts = (const char (*)[AT_HOST_MAX + 1])m->cfg.hosts;
    st->n_log = 0;
    for (const struct at_log_entry *e; st->n_log < AT_LOG_MAX && (e = at_modem_log_get(m, (unsigned)st->n_log)); )
        st->log[st->n_log++] = e;
    st->now_ms = now_ms();
}

static err_t on_http_recv(void *arg, struct tcp_pcb *p, struct pbuf *buf, err_t err)
{
    struct http_conn *c = arg;
    if (!buf) { conn_close(c); return ERR_OK; }
    if (err != ERR_OK || c->resp) { tcp_recved(p, buf->tot_len); pbuf_free(buf); return ERR_OK; }
    size_t room = HTTP_REQ_MAX - c->req_len;
    size_t n = buf->tot_len < room ? buf->tot_len : room;
    pbuf_copy_partial(buf, c->req + c->req_len, (u16_t)n, 0);
    c->req_len += n;
    tcp_recved(p, buf->tot_len);
    pbuf_free(buf);
    last_http_ms = now_ms();

    struct web_status st;
    char sta_ip[16];
    status_snapshot(&st, sta_ip, sizeof sta_ip);
    char *resp = malloc(HTTP_RESP_MAX);
    if (!resp) { conn_close(c); return ERR_OK; }
    struct web_form f;
    static struct web_hosts h;          /* rappels lwIP non réentrants */
    size_t len = 0;
    enum web_result r = web_setup_handle(c->req, c->req_len, &st, &f, &h, resp, HTTP_RESP_MAX, &len);
    if (r == WEB_INCOMPLETE) {
        free(resp);
        if (c->req_len == HTTP_REQ_MAX) conn_close(c);   /* requête trop grande */
        return ERR_OK;
    }
    if (r == WEB_REPLY_SUBMIT) {
        form = f;
        strcpy(join_ssid, f.ssid);
        join_state = WEB_JOIN_RUNNING;
        form_pending = true;
    } else if (r == WEB_REPLY_RESCAN) {
        scan_pending = true;
    } else if (r == WEB_REPLY_HOSTS) {
        hosts_form = h;
        hosts_pending = true;
    }
    c->resp = resp;
    c->resp_len = len;
    send_more(c);
    return ERR_OK;
}

static err_t on_http_sent(void *arg, struct tcp_pcb *p, u16_t len)
{
    (void)p; (void)len;
    struct http_conn *c = arg;
    if (c && c->resp) send_more(c);
    return ERR_OK;
}

static err_t on_http_poll(void *arg, struct tcp_pcb *p)
{
    (void)p;
    conn_close(arg);                    /* connexion inactive depuis 10 s */
    return ERR_OK;
}

static void on_http_err(void *arg, err_t err)
{
    (void)err;
    if (arg) conn_free(arg);            /* pcb déjà libéré par lwIP */
}

static err_t on_http_accept(void *arg, struct tcp_pcb *p, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !p) return ERR_VAL;
    struct http_conn *c = NULL;
    for (int i = 0; i < HTTP_CONNS; i++) if (!conns[i].pcb) { c = &conns[i]; break; }
    if (!c) { tcp_abort(p); return ERR_ABRT; }
    memset(c, 0, sizeof *c);
    c->pcb = p;
    tcp_arg(p, c);
    tcp_recv(p, on_http_recv);
    tcp_sent(p, on_http_sent);
    tcp_err(p, on_http_err);
    tcp_poll(p, on_http_poll, 20);      /* 20 × 500 ms */
    return ERR_OK;
}

/* ------------------------------------------------------------ DHCP, DNS */

static void reply_udp(struct udp_pcb *pcb, const uint8_t *data, size_t len,
                      const ip_addr_t *addr, u16_t port)
{
    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!q) return;
    memcpy(q->payload, data, len);
    udp_sendto_if(pcb, q, addr, port, &cyw43_state.netif[CYW43_ITF_AP]);
    pbuf_free(q);
}

static void on_dhcp(void *arg, struct udp_pcb *pcb, struct pbuf *buf, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)addr; (void)port;
    static uint8_t in[600], out[DHCPS_REPLY_MAX];
    u16_t n = pbuf_copy_partial(buf, in, sizeof in, 0);
    pbuf_free(buf);
    size_t r = dhcps_handle(&dhcps, in, n, out, sizeof out, now_ms() / 1000);
    if (r) reply_udp(pcb, out, r, IP_ADDR_BROADCAST, 68);
}

static void on_dns(void *arg, struct udp_pcb *pcb, struct pbuf *buf, const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    static uint8_t in[512], out[512];
    u16_t n = pbuf_copy_partial(buf, in, sizeof in, 0);
    pbuf_free(buf);
    size_t r = dns_catchall(in, n, out, sizeof out, dhcps.ip);
    if (r) reply_udp(pcb, out, r, addr, port);
}

/* ----------------------------------------------------- ouverture, fermeture */

static void ap_close(void)
{
    if (!active) return;
    cyw43_arch_lwip_begin();
    for (int i = 0; i < HTTP_CONNS; i++) if (conns[i].pcb) conn_close(&conns[i]);
    if (http_pcb) { tcp_close(http_pcb); http_pcb = NULL; }
    if (dhcp_pcb) { udp_remove(dhcp_pcb); dhcp_pcb = NULL; }
    if (dns_pcb) { udp_remove(dns_pcb); dns_pcb = NULL; }
    cyw43_arch_lwip_end();
    cyw43_arch_disable_ap_mode();
    active = false;
}

static int ap_open(void)
{
    if (active) return AT_NET_OK;
    struct at_modem *m = net_pico_modem();
    uint8_t mac[6];
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    snprintf(ap_ssid, sizeof ap_ssid, "Neo6502-modem-%02X%02X", mac[4], mac[5]);
    cyw43_arch_enable_ap_mode(ap_ssid, m->cfg.ap_pass, CYW43_AUTH_WPA2_AES_PSK);

    struct netif *ap = &cyw43_state.netif[CYW43_ITF_AP];
    int r = AT_NET_OK;
    cyw43_arch_lwip_begin();
    /* le pilote fait du point d'accès l'interface par défaut : les connexions
       sortantes (CIPSTART, DNS, SNTP) doivent rester sur la station */
    netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
    dhcps_init(&dhcps, lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(ap))),
               lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(ap))));
    ip4addr_ntoa_r(netif_ip4_addr(ap), ap_ip_str, sizeof ap_ip_str);

    dhcp_pcb = udp_new();
    dns_pcb = udp_new();
    http_pcb = tcp_new();
    if (!dhcp_pcb || !dns_pcb || !http_pcb) r = AT_NET_FAIL;
    if (r == AT_NET_OK) {
        udp_bind_netif(dhcp_pcb, ap);
        udp_bind_netif(dns_pcb, ap);
        tcp_bind_netif(http_pcb, ap);
        if (udp_bind(dhcp_pcb, IP_ANY_TYPE, 67) != ERR_OK
            || udp_bind(dns_pcb, IP_ANY_TYPE, 53) != ERR_OK
            || tcp_bind(http_pcb, IP_ANY_TYPE, 80) != ERR_OK) r = AT_NET_FAIL;
    }
    if (r == AT_NET_OK) {
        udp_recv(dhcp_pcb, on_dhcp, NULL);
        udp_recv(dns_pcb, on_dns, NULL);
        struct tcp_pcb *l = tcp_listen_with_backlog(http_pcb, HTTP_CONNS);
        if (l) { http_pcb = l; tcp_accept(l, on_http_accept); } else r = AT_NET_FAIL;
    }
    cyw43_arch_lwip_end();
    active = true;
    if (r != AT_NET_OK) { ap_close(); return r; }
    last_http_ms = now_ms();
    ok_ms = 0;
    join_state = WEB_JOIN_IDLE;
    scan_pending = true;                 /* liste des réseaux dès l'ouverture */
    return AT_NET_OK;
}

int ap_pico_setup(void *ctx, int on)
{
    (void)ctx;
    if (!net_pico_ready()) return AT_NET_FAIL;
    if (on) return ap_open();
    ap_close();
    return AT_NET_OK;
}

const char *ap_pico_ssid(void *ctx)
{
    (void)ctx;
    return active ? ap_ssid : NULL;
}

/* ------------------------------------------------------------ boucle */

static struct web_ap scan_tmp[WEB_AP_MAX];
static int scan_tmp_n;
static uint32_t scan_t0;

static void scan_cb(void *ctx, int ecn, const char *ssid, int rssi)
{
    (void)ctx;
    if (scan_tmp_n >= WEB_AP_MAX) return;
    snprintf(scan_tmp[scan_tmp_n].ssid, sizeof scan_tmp[0].ssid, "%s", ssid);
    scan_tmp[scan_tmp_n].ecn = ecn;
    scan_tmp[scan_tmp_n].rssi = rssi;
    scan_tmp_n++;
}

static void poll_join(void)
{
    if (join_state != WEB_JOIN_RUNNING) return;
    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (st == CYW43_LINK_UP && !strcmp(net_pico_joined_ssid(), join_ssid)) {
        join_state = WEB_JOIN_OK;
        ok_ms = now_ms();
        return;
    }
    if (st == CYW43_LINK_BADAUTH) { join_state = WEB_JOIN_BAD_PASSWORD; return; }
    if (st == CYW43_LINK_NONET) join_nonet = true;
    if (now_ms() - join_t0 > 30000) join_state = join_nonet ? WEB_JOIN_NO_AP : WEB_JOIN_FAIL;
}

void ap_pico_poll(void)
{
    struct at_modem *m = net_pico_modem();
    uint32_t now = now_ms();

    /* ouverture automatique (une fois par démarrage) */
    if (!auto_checked) {
        if (!m->cfg.ssid[0]) { auto_checked = true; ap_open(); }
        else if (now >= AP_AUTO_OPEN_MS) {
            auto_checked = true;
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) ap_open();
        }
    }
    if (!active) return;

    if (form_pending) {
        struct web_form f;
        cyw43_arch_lwip_begin();
        f = form;
        form_pending = false;
        cyw43_arch_lwip_end();
        /* même effet que AT+CWJAP_DEF, sans bloquer : enregistrement puis
           reconnexion de fond vers le nouveau réseau */
        strcpy(m->cfg.ssid, f.ssid);
        strcpy(m->cfg.pass, f.pass);
        config_flash_save(NULL, &m->cfg);
        join_t0 = now_ms();
        join_nonet = false;
        net_pico_join_saved();
    }
    poll_join();

    if (hosts_pending) {
        /* US-T12 : seul chemin d'écriture de la liste (la commande AT est en lecture seule) */
        cyw43_arch_lwip_begin();
        struct web_hosts h = hosts_form;
        hosts_pending = false;
        cyw43_arch_lwip_end();
        m->cfg.hosts_enforce = (uint8_t)(h.enforce ? 1 : 0);
        memcpy(m->cfg.hosts, h.hosts, sizeof m->cfg.hosts);
        if (m->cfg.hosts_enforce && m->cfg.listen_port) {
            m->cfg.listen_port = 0;                     /* appels entrants refusés */
            (net_pico_ops.tcp_listen)(NULL, 0);         /* parenthèses : macro lwIP tcp_listen */
        }
        config_flash_save(NULL, &m->cfg);
    }

    /* recherche des réseaux sans bloquer les commandes AT */
    if (scan_pending && !scanning && net_pico_scan_start()) {
        scanning = true;
        scan_pending = false;
        scan_t0 = now_ms();
    }
    if (scanning && (!net_pico_scan_busy() || now_ms() - scan_t0 > 15000)) {
        scan_tmp_n = 0;
        net_pico_scan_results(scan_cb, NULL);
        cyw43_arch_lwip_begin();
        memcpy(aps, scan_tmp, sizeof aps);
        n_ap = scan_tmp_n;
        scanning = false;
        cyw43_arch_lwip_end();
    }

    now = now_ms();
    if (ok_ms && now - ok_ms >= AP_OK_CLOSE_MS) ap_close();
    else if (now - last_http_ms >= AP_IDLE_CLOSE_MS) ap_close();
}
