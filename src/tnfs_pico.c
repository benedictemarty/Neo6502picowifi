/*
 * tnfs_pico.c — second port USB (CDC 1) réservé à TNFS, sur Pico W.
 *
 * Chaque trame reçue sur le port (tnfs_link.h) part en un datagramme UDP vers
 * le serveur AT$TNFS ; chaque datagramme du serveur revient en une trame.
 * Lien UDP propre, indépendant du lien AT (CIPSTART/ATDT) : une session
 * Minitel ou Telnet et TNFS peuvent tourner en même temps. Sans Wi-Fi, sans
 * serveur, pendant la résolution DNS ou port fermé : rien n'est renvoyé, le
 * client TNFS gère ses délais et ses répétitions.
 */
#include "tnfs_pico.h"
#include "tnfs_link.h"
#include "net_pico.h"

#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/udp.h"
#include "tusb.h"

#define TNFS_ITF 1                       /* numéro d'instance CDC TinyUSB */

static struct tnfs_rx rx;
static struct tnfs_queue q;
static struct udp_pcb *pcb;

static char cur_host[AT_HOST_MAX + 1];
static uint16_t cur_port;
static ip_addr_t addr;
static enum { RES_NONE, RES_PENDING, RES_OK, RES_FAIL } volatile res;
static volatile unsigned res_gen;        /* invalide une résolution devenue obsolète */
static uint8_t pending[TNFS_DGRAM_MAX];  /* dernier datagramme reçu pendant le DNS */
static size_t pending_len;

static void on_udp(void *arg, struct udp_pcb *p, struct pbuf *buf, const ip_addr_t *a, u16_t port)
{
    static uint8_t d[TNFS_DGRAM_MAX];
    (void)arg; (void)p; (void)a; (void)port;
    if (buf->tot_len <= TNFS_DGRAM_MAX) {
        u16_t n = pbuf_copy_partial(buf, d, buf->tot_len, 0);
        tnfs_queue_push(&q, d, n);
    } else {
        q.dropped++;
    }
    pbuf_free(buf);
}

static void on_dns_found(const char *name, const ip_addr_t *found, void *arg)
{
    (void)name;
    if ((unsigned)(uintptr_t)arg != res_gen) return;
    if (found) { addr = *found; res = RES_OK; } else res = RES_FAIL;
}

/* lien UDP vers addr:cur_port (appelé sous cyw43_arch_lwip_begin) */
static bool link_up(void)
{
    if (pcb) return true;
    pcb = udp_new_ip_type(IP_GET_TYPE(&addr));
    if (!pcb) return false;
    if (udp_bind(pcb, IP_ANY_TYPE, 0) != ERR_OK || udp_connect(pcb, &addr, cur_port) != ERR_OK) {
        udp_remove(pcb);
        pcb = NULL;
        return false;
    }
    udp_recv(pcb, on_udp, NULL);
    return true;
}

static void send_dgram(const uint8_t *d, size_t len)
{
    cyw43_arch_lwip_begin();
    if (link_up()) {
        struct pbuf *b = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (b) {
            memcpy(b->payload, d, len);
            udp_send(pcb, b);
            pbuf_free(b);
        }
    }
    cyw43_arch_lwip_end();
}

static void reset_link(void)
{
    cyw43_arch_lwip_begin();
    if (pcb) { udp_remove(pcb); pcb = NULL; }
    res_gen++;
    res = RES_NONE;
    cyw43_arch_lwip_end();
    pending_len = 0;
    tnfs_queue_clear(&q);
}

static void on_frame(void *ctx, const uint8_t *d, size_t len)
{
    (void)ctx;
    struct at_modem *m = net_pico_modem();
    if (!m->cfg.tnfs_host[0] || !net_pico_ops.wifi_connected(NULL)) return;
    if (strcmp(cur_host, m->cfg.tnfs_host) || cur_port != m->cfg.tnfs_port) {
        reset_link();                    /* AT$TNFS a changé */
        strcpy(cur_host, m->cfg.tnfs_host);
        cur_port = m->cfg.tnfs_port;
    }
    if (res == RES_OK) { send_dgram(d, len); return; }
    memcpy(pending, d, len);             /* envoyé dès que l'adresse est connue */
    pending_len = len;
    if (res == RES_PENDING) return;
    if (ipaddr_aton(cur_host, &addr)) { res = RES_OK; return; }
    cyw43_arch_lwip_begin();
    unsigned gen = ++res_gen;
    res = RES_PENDING;
    err_t e = dns_gethostbyname(cur_host, &addr, on_dns_found, (void *)(uintptr_t)gen);
    if (e == ERR_OK) res = RES_OK;
    else if (e != ERR_INPROGRESS) res = RES_FAIL;   /* nouvel essai à la prochaine trame */
    cyw43_arch_lwip_end();
}

void tnfs_pico_init(void)
{
    tnfs_rx_init(&rx);
    tnfs_queue_init(&q);
}

void tnfs_pico_poll(void)
{
    uint8_t buf[256];
    while (tud_cdc_n_available(TNFS_ITF)) {
        uint32_t n = tud_cdc_n_read(TNFS_ITF, buf, sizeof buf);
        if (!n) break;
        tnfs_rx_feed(&rx, buf, n, on_frame, NULL);
    }
    if (res == RES_OK && pending_len) {
        send_dgram(pending, pending_len);
        pending_len = 0;
    }
    if (!tud_cdc_n_connected(TNFS_ITF)) {   /* port fermé (DTR) : personne pour lire */
        tnfs_queue_clear(&q);
        return;
    }
    static uint8_t frame[2 + TNFS_DGRAM_MAX];
    bool wrote = false;
    for (size_t n; (n = tnfs_queue_peek(&q)) && tud_cdc_n_write_available(TNFS_ITF) >= n; wrote = true) {
        tnfs_queue_pop(&q, frame, sizeof frame);
        tud_cdc_n_write(TNFS_ITF, frame, (uint32_t)n);
    }
    if (wrote) tud_cdc_n_write_flush(TNFS_ITF);
}
