/*
 * net_pico.c — Wi-Fi, TCP, DNS, SNTP, ping et flash pour le modem sur Pico W.
 *
 * Mode pico_cyw43_arch_lwip_threadsafe_background : les rappels lwIP
 * s'exécutent en interruption ; tout appel lwIP depuis la boucle principale
 * est encadré par cyw43_arch_lwip_begin/end. Une seule connexion TCP
 * (CIPMUX=0) + une écoute entrante (CIPSERVER / ATA).
 */
#include "net_pico.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/icmp.h"
#include "lwip/raw.h"
#include "lwip/inet_chksum.h"
#include "lwip/apps/sntp.h"

#ifndef PICOW_MODEM_VERSION
#define PICOW_MODEM_VERSION "0.1.0"
#endif

#define CONNECT_TIMEOUT_MS 10000
#define SEND_TIMEOUT_MS    5000
#define RING_PERIOD_MS     3000

static struct at_modem *modem;
static struct tcp_pcb *pcb;            /* connexion active                  */
static struct tcp_pcb *listen_pcb;     /* écoute                            */
static struct tcp_pcb *pending_pcb;    /* appel entrant non répondu         */
static volatile int connect_state;     /* 0 en cours, 1 ok, <0 erreur       */
static volatile bool dns_done;
static ip_addr_t dns_result;
static char joined_ssid[AT_SSID_MAX + 1];
static uint32_t last_ring_ms;
static volatile time_t sntp_epoch;     /* dernier temps SNTP reçu (0 = aucun) */
static volatile uint32_t sntp_at_ms;

/* ------------------------------------------------------------ flash */

#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

void config_flash_load(struct at_config *cfg)
{
    const struct at_config *f = (const struct at_config *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    memcpy(cfg, f, sizeof *cfg);
}

void config_flash_save(void *ctx, const struct at_config *cfg)
{
    (void)ctx;
    static uint8_t page[FLASH_PAGE_SIZE * ((sizeof(struct at_config) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE)];
    memset(page, 0xff, sizeof page);
    memcpy(page, cfg, sizeof *cfg);
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page, sizeof page);
    restore_interrupts(irq);
    /* AT+CIPSNTPCFG prend effet immédiatement si le Wi-Fi est déjà associé */
    if (modem && cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
        apply_sntp_config();
}

/* ------------------------------------------------------------ Wi-Fi */

static void apply_sntp_config(void)
{
    cyw43_arch_lwip_begin();
    sntp_stop();
    if (modem->cfg.sntp_enable) {
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, modem->cfg.sntp_server);
        sntp_init();
    }
    cyw43_arch_lwip_end();
}

static void apply_ip_config(void)
{
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    cyw43_arch_lwip_begin();
    if (!modem->cfg.dhcp && modem->cfg.static_ip[0]) {
        ip4_addr_t ip, gw, mask;
        ip4addr_aton(modem->cfg.static_ip, &ip);
        ip4addr_aton(modem->cfg.static_gw, &gw);
        ip4addr_aton(modem->cfg.static_mask, &mask);
        dhcp_stop(n);
        netif_set_addr(n, &ip, &mask, &gw);
    }
    if (modem->cfg.dns[0]) {
        ip_addr_t d;
        if (ipaddr_aton(modem->cfg.dns, &d)) dns_setserver(0, &d);
    }
    cyw43_arch_lwip_end();
    apply_sntp_config();
}

static int wifi_join(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    uint32_t auth = pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
    int r = cyw43_arch_wifi_connect_timeout_ms(ssid, pass[0] ? pass : NULL, auth, 30000);
    if (r == 0) {
        strncpy(joined_ssid, ssid, AT_SSID_MAX);
        apply_ip_config();
        return AT_NET_OK;
    }
    if (r == PICO_ERROR_TIMEOUT) return AT_NET_TIMEOUT;
    if (r == PICO_ERROR_BADAUTH) return AT_NET_BAD_PASSWORD;
    int st = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    if (st == CYW43_LINK_NONET) return AT_NET_NO_AP;
    return AT_NET_FAIL;
}

static void wifi_leave(void *ctx)
{
    (void)ctx;
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    joined_ssid[0] = 0;
}

static bool wifi_connected(void *ctx)
{
    (void)ctx;
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

/* Un SSID diffusé par plusieurs points d'accès n'est listé qu'une fois
   (meilleur RSSI), sinon netsetup (MAX_NETWORKS) se remplit de doublons. */
#define SCAN_MAX 24
struct scan_ctx {
    at_scan_cb cb; void *cb_ctx;
    int n;
    struct { char ssid[33]; int ecn, rssi; } ap[SCAN_MAX];
};

static int scan_result(void *env, const cyw43_ev_scan_result_t *r)
{
    struct scan_ctx *s = env;
    if (!r || r->ssid_len == 0) return 0;
    char ssid[33];
    memcpy(ssid, r->ssid, r->ssid_len);
    ssid[r->ssid_len] = 0;
    /* auth_mode : bit 0 WEP, bit 1 WPA, bit 2 WPA2 (valeurs cyw43) ; ecn ESP :
       0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2 */
    int ecn = 0;
    if ((r->auth_mode & 6) == 6) ecn = 4;
    else if (r->auth_mode & 4) ecn = 3;
    else if (r->auth_mode & 2) ecn = 2;
    else if (r->auth_mode & 1) ecn = 1;
    for (int i = 0; i < s->n; i++) {
        if (!strcmp(s->ap[i].ssid, ssid)) {
            if (r->rssi > s->ap[i].rssi) s->ap[i].rssi = r->rssi;
            return 0;
        }
    }
    if (s->n < SCAN_MAX) {
        strcpy(s->ap[s->n].ssid, ssid);
        s->ap[s->n].ecn = ecn;
        s->ap[s->n].rssi = r->rssi;
        s->n++;
    }
    return 0;
}

static int wifi_scan(void *ctx, at_scan_cb cb, void *cb_ctx)
{
    (void)ctx;
    static struct scan_ctx s;
    memset(&s, 0, sizeof s);
    s.cb = cb; s.cb_ctx = cb_ctx;
    cyw43_wifi_scan_options_t opts = { 0 };
    if (cyw43_wifi_scan(&cyw43_state, &opts, &s, scan_result) != 0) return AT_NET_FAIL;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (cyw43_wifi_scan_active(&cyw43_state) && to_ms_since_boot(get_absolute_time()) - t0 < 15000)
        sleep_ms(50);
    /* tri par RSSI décroissant (comme AT+CWLAPOPT=1,…) */
    for (int i = 1; i < s.n; i++)
        for (int j = i; j > 0 && s.ap[j].rssi > s.ap[j - 1].rssi; j--) {
            typeof(s.ap[0]) t = s.ap[j]; s.ap[j] = s.ap[j - 1]; s.ap[j - 1] = t;
        }
    for (int i = 0; i < s.n; i++) cb(cb_ctx, s.ap[i].ecn, s.ap[i].ssid, s.ap[i].rssi);
    return AT_NET_OK;
}

static void ip_info(void *ctx, struct at_ip_info *info)
{
    (void)ctx;
    memset(info, 0, sizeof *info);
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    cyw43_arch_lwip_begin();
    ip4addr_ntoa_r(netif_ip4_addr(n), info->ip, sizeof info->ip);
    ip4addr_ntoa_r(netif_ip4_gw(n), info->gateway, sizeof info->gateway);
    ip4addr_ntoa_r(netif_ip4_netmask(n), info->netmask, sizeof info->netmask);
    ipaddr_ntoa_r(dns_getserver(0), info->dns, sizeof info->dns);
    cyw43_arch_lwip_end();
    uint8_t mac[6];
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    snprintf(info->mac, sizeof info->mac, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    strcpy(info->ssid, joined_ssid);
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    info->rssi = rssi;
    uint32_t ch = 0;
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof ch, (uint8_t *)&ch, CYW43_ITF_STA) == 0)
        info->channel = (int)ch;
    info->dhcp = modem->cfg.dhcp;
}

/* -------------------------------------------------------------- TCP */

static void tcp_detach(struct tcp_pcb *p)
{
    tcp_arg(p, NULL);
    tcp_recv(p, NULL);
    tcp_err(p, NULL);
    tcp_sent(p, NULL);
    if (tcp_close(p) != ERR_OK) tcp_abort(p);
}

static err_t on_recv(void *arg, struct tcp_pcb *p, struct pbuf *buf, err_t err)
{
    (void)arg; (void)err;
    if (!buf) {                                   /* fermeture distante */
        at_modem_remote_closed(modem);
        if (p == pcb) { tcp_detach(p); pcb = NULL; }
        return ERR_OK;
    }
    if (at_modem_rx_space(modem) < buf->tot_len) return ERR_MEM; /* lwIP réessaie */
    for (struct pbuf *q = buf; q; q = q->next) at_modem_rx_push(modem, q->payload, q->len);
    tcp_recved(p, buf->tot_len);
    pbuf_free(buf);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    /* le pcb est déjà libéré par lwIP */
    if (connect_state == 0) connect_state = -1;
    pcb = NULL;
    at_modem_remote_closed(modem);
}

static err_t on_connected(void *arg, struct tcp_pcb *p, err_t err)
{
    (void)arg; (void)p;
    connect_state = (err == ERR_OK) ? 1 : -1;
    return ERR_OK;
}

static void on_dns(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name; (void)arg;
    if (addr) dns_result = *addr; else ip_addr_set_zero(&dns_result);
    dns_done = true;
}

static bool resolve(const char *host, ip_addr_t *out)
{
    if (ipaddr_aton(host, out)) return true;
    dns_done = false;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, out, on_dns, NULL);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) return true;
    if (e != ERR_INPROGRESS) return false;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (!dns_done && to_ms_since_boot(get_absolute_time()) - t0 < CONNECT_TIMEOUT_MS) sleep_ms(1);
    if (!dns_done || ip_addr_isany(&dns_result)) return false;
    *out = dns_result;
    return true;
}

static int tcp_connect_op(void *ctx, const char *host, uint16_t port)
{
    (void)ctx;
    ip_addr_t addr;
    if (!resolve(host, &addr)) return AT_NET_DNS_FAIL;
    cyw43_arch_lwip_begin();
    struct tcp_pcb *p = tcp_new_ip_type(IP_GET_TYPE(&addr));
    if (!p) { cyw43_arch_lwip_end(); return AT_NET_FAIL; }
    tcp_recv(p, on_recv);
    tcp_err(p, on_err);
    connect_state = 0;
    pcb = p;
    err_t e = tcp_connect(p, &addr, port, on_connected);
    cyw43_arch_lwip_end();
    if (e != ERR_OK) { cyw43_arch_lwip_begin(); tcp_detach(p); cyw43_arch_lwip_end(); pcb = NULL; return AT_NET_CONNECT_FAIL; }
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (connect_state == 0 && to_ms_since_boot(get_absolute_time()) - t0 < CONNECT_TIMEOUT_MS) sleep_ms(1);
    if (connect_state != 1) {
        cyw43_arch_lwip_begin();
        if (pcb) tcp_detach(pcb);
        cyw43_arch_lwip_end();
        pcb = NULL;
        modem->remote_closed = false;
        return AT_NET_CONNECT_FAIL;
    }
    return AT_NET_OK;
}

static int tcp_send_op(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (len) {
        if (!pcb) return AT_NET_FAIL;
        cyw43_arch_lwip_begin();
        size_t chunk = tcp_sndbuf(pcb);
        if (chunk > len) chunk = len;
        err_t e = ERR_OK;
        if (chunk) {
            e = tcp_write(pcb, data, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) tcp_output(pcb);
        }
        cyw43_arch_lwip_end();
        if (e == ERR_OK && chunk) { data += chunk; len -= chunk; t0 = to_ms_since_boot(get_absolute_time()); continue; }
        if (e != ERR_OK && e != ERR_MEM) return AT_NET_FAIL;
        if (to_ms_since_boot(get_absolute_time()) - t0 > SEND_TIMEOUT_MS) return AT_NET_TIMEOUT;
        sleep_ms(1);
    }
    return AT_NET_OK;
}

static void tcp_close_op(void *ctx)
{
    (void)ctx;
    cyw43_arch_lwip_begin();
    if (pcb) { tcp_detach(pcb); pcb = NULL; }
    cyw43_arch_lwip_end();
}

static bool tcp_connected_op(void *ctx)
{
    (void)ctx;
    return pcb != NULL && connect_state == 1;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    if (pcb || pending_pcb) { tcp_abort(newpcb); return ERR_ABRT; } /* occupé */
    pending_pcb = newpcb;
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);
    last_ring_ms = to_ms_since_boot(get_absolute_time());
    at_modem_ring(modem);
    return ERR_OK;
}

static int tcp_listen_op(void *ctx, uint16_t port)
{
    (void)ctx;
    cyw43_arch_lwip_begin();
    if (listen_pcb) { tcp_close(listen_pcb); listen_pcb = NULL; }
    int r = AT_NET_OK;
    if (port) {
        struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_ANY);
        if (!p || tcp_bind(p, IP_ANY_TYPE, port) != ERR_OK) { if (p) tcp_close(p); r = AT_NET_FAIL; }
        else {
            listen_pcb = tcp_listen_with_backlog(p, 1);
            if (!listen_pcb) r = AT_NET_FAIL; else tcp_accept(listen_pcb, on_accept);
        }
    }
    cyw43_arch_lwip_end();
    return r;
}

static bool tcp_accept_op(void *ctx)
{
    (void)ctx;
    cyw43_arch_lwip_begin();
    bool ok = pending_pcb != NULL;
    if (ok) { pcb = pending_pcb; pending_pcb = NULL; connect_state = 1; modem->remote_closed = false; }
    cyw43_arch_lwip_end();
    return ok;
}

/* ------------------------------------------------------- SNTP/ping */

/* lwipopts.h : SNTP_SET_SYSTEM_TIME(sec) → net_pico_set_time */
void net_pico_set_time(unsigned int sec)
{
    sntp_epoch = (time_t)sec;
    sntp_at_ms = to_ms_since_boot(get_absolute_time());
}

static void sntp_time_op(void *ctx, char *out, size_t out_len)
{
    (void)ctx;
    out[0] = 0;
    if (!sntp_epoch) return;
    time_t t = sntp_epoch + (to_ms_since_boot(get_absolute_time()) - sntp_at_ms) / 1000
             + (time_t)modem->cfg.sntp_tz * 3600;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, out_len, "%a %b %d %H:%M:%S %Y", &tm);
}

static volatile int ping_reply_ms;
static uint32_t ping_sent_ms;
static uint16_t ping_seq;

static u8_t on_ping(void *arg, struct raw_pcb *p, struct pbuf *buf, const ip_addr_t *addr)
{
    (void)arg; (void)p; (void)addr;
    if (buf->tot_len >= (u16_t)(PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr))
        && pbuf_remove_header(buf, PBUF_IP_HLEN) == 0) {
        struct icmp_echo_hdr *h = buf->payload;
        if (ICMPH_TYPE(h) == ICMP_ER && h->seqno == lwip_htons(ping_seq)) {
            ping_reply_ms = (int)(to_ms_since_boot(get_absolute_time()) - ping_sent_ms);
            pbuf_free(buf);
            return 1;
        }
        pbuf_add_header(buf, PBUF_IP_HLEN);
    }
    return 0;
}

static int ping_op(void *ctx, const char *host)
{
    (void)ctx;
    ip_addr_t addr;
    if (!resolve(host, &addr) || !IP_IS_V4(&addr)) return -1;
    cyw43_arch_lwip_begin();
    struct raw_pcb *p = raw_new(IP_PROTO_ICMP);
    if (!p) { cyw43_arch_lwip_end(); return -1; }
    raw_recv(p, on_ping, NULL);
    raw_bind(p, IP_ADDR_ANY);
    struct pbuf *q = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 32, PBUF_RAM);
    int r = -1;
    if (q) {
        struct icmp_echo_hdr *h = q->payload;
        ICMPH_TYPE_SET(h, ICMP_ECHO);
        ICMPH_CODE_SET(h, 0);
        h->chksum = 0;
        h->id = lwip_htons(0x6502);
        h->seqno = lwip_htons(++ping_seq);
        memset((uint8_t *)h + sizeof *h, 'N', 32);
        h->chksum = inet_chksum(h, q->len);
        ping_reply_ms = -1;
        ping_sent_ms = to_ms_since_boot(get_absolute_time());
        raw_sendto(p, q, &addr);
        pbuf_free(q);
        r = 0;
    }
    cyw43_arch_lwip_end();
    if (r == 0) {
        while (ping_reply_ms < 0 && to_ms_since_boot(get_absolute_time()) - ping_sent_ms < 3000) sleep_ms(1);
        r = ping_reply_ms;
    }
    cyw43_arch_lwip_begin();
    raw_remove(p);
    cyw43_arch_lwip_end();
    return r;
}

static void reset_op(void *ctx)
{
    (void)ctx;
    sleep_ms(50);
    watchdog_reboot(0, 0, 10);
    while (1) tight_loop_contents();
}

static void bootsel_op(void *ctx)
{
    (void)ctx;
    sleep_ms(50);
    reset_usb_boot(0, 0);
}

static const char *version_op(void *ctx) { (void)ctx; return PICOW_MODEM_VERSION; }

/* ------------------------------------------------------------- init */

struct at_modem_ops net_pico_ops = {
    .ctx = NULL,
    .write = NULL, .millis = NULL,          /* fournis par main.c */
    .wifi_join = wifi_join, .wifi_leave = wifi_leave, .wifi_connected = wifi_connected,
    .wifi_scan = wifi_scan, .ip_info = ip_info,
    .tcp_connect = tcp_connect_op, .tcp_send = tcp_send_op, .tcp_close = tcp_close_op,
    .tcp_connected = tcp_connected_op, .tcp_listen = tcp_listen_op, .tcp_accept = tcp_accept_op,
    .config_save = config_flash_save, .sntp_time = sntp_time_op, .ping = ping_op,
    .reset = reset_op, .bootsel = bootsel_op, .version = version_op,
};

bool net_pico_init(struct at_modem *m)
{
    modem = m;
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_FRANCE) != 0) return false;
    cyw43_arch_enable_sta_mode();
    return true;
}

void net_pico_poll(void)
{
    /* sonnerie répétée tant que l'appel entrant n'est pas décroché */
    if (pending_pcb && to_ms_since_boot(get_absolute_time()) - last_ring_ms >= RING_PERIOD_MS) {
        last_ring_ms = to_ms_since_boot(get_absolute_time());
        at_modem_ring(modem);
    }
}
