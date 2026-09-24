/*
 * net_pico.c — Wi-Fi, TCP, DNS, SNTP, ping et flash pour le modem sur Pico W.
 *
 * Mode pico_cyw43_arch_lwip_threadsafe_background : les rappels lwIP
 * s'exécutent en interruption ; tout appel lwIP depuis la boucle principale
 * est encadré par cyw43_arch_lwip_begin/end. Une seule connexion TCP
 * (CIPMUX=0) + une écoute entrante (CIPSERVER / ATA).
 */
#include "net_pico.h"
#include "tls_date.h"
#include "roots_ca_cb.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/debug.h"
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "mbedtls/platform_time.h"
#include <sys/time.h>
#include "lwip/icmp.h"
#include "lwip/raw.h"
#include "lwip/inet_chksum.h"
#include "lwip/apps/sntp.h"

#ifndef PICOW_MODEM_VERSION
#define PICOW_MODEM_VERSION "0.1.0"
#endif

#define CONNECT_TIMEOUT_MS 10000
#define TLS_CONNECT_TIMEOUT_MS 30000
#define SEND_TIMEOUT_MS    5000
#define RING_PERIOD_MS     3000

static struct at_modem *modem;
static volatile bool dns_done;
static ip_addr_t dns_result;
static char joined_ssid[AT_SSID_MAX + 1];
static uint32_t last_ring_ms;
static volatile time_t sntp_epoch;     /* dernier temps SNTP reçu (0 = aucun) */
static volatile uint32_t sntp_at_ms;

/* ------------------------------------------------------------ flash */

#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

static void apply_sntp_config(void);

/* Attente qui rafraîchit le watchdog ; stage = point d'étape conservé dans
   un registre scratch (lu au boot après un reset watchdog, cf. main.c). */
static void wait_ms(uint32_t ms) { watchdog_update(); sleep_ms(ms); }
void net_pico_stage(uint32_t stage) { watchdog_hw->scratch[4] = stage; }

/* Derniers messages de diagnostic lwIP (LWIP_PLATFORM_DIAG), pour ATI. */
static char diag_buf[6][80];
static volatile unsigned diag_n;
void net_pico_diag(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(diag_buf[diag_n % 6], sizeof diag_buf[0], fmt, ap);
    va_end(ap);
    char *e = diag_buf[diag_n % 6] + strlen(diag_buf[diag_n % 6]);
    while (e > diag_buf[diag_n % 6] && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    diag_n++;
}

void net_pico_lwip_assert(const char *msg)
{
    watchdog_hw->scratch[5] = (uint32_t)msg;   /* chaîne en flash : lisible après reset */
    watchdog_hw->scratch[6] = 0x4C574950;      /* 'LWIP' */
    while (1) tight_loop_contents();           /* le watchdog redémarre la carte */
}

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

/* Reconnexion de fond (comme l'ESP8266) : tant que le SSID mémorisé n'est pas
   associé, une tentative asynchrone toutes les BG_RETRY_MS, sans bloquer les
   commandes AT ; l'IP/DNS/SNTP sont appliqués quand le lien monte. */
#define BG_RETRY_MS 15000
static bool bg_enabled, bg_was_up;
static uint32_t bg_last_try;

static uint32_t auth_for(const char *pass) { return pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN; }

void net_pico_background_join(bool enable)
{
    bg_enabled = enable;
    bg_last_try = to_ms_since_boot(get_absolute_time()) - BG_RETRY_MS; /* première tentative immédiate */
}

static void bg_poll(void)
{
    if (!bg_enabled || !modem->cfg.ssid[0]) return;
    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (st == CYW43_LINK_UP) {
        if (!bg_was_up) { memcpy(joined_ssid, modem->cfg.ssid, sizeof joined_ssid); apply_ip_config(); }
        bg_was_up = true;
        return;
    }
    bg_was_up = false;
    if (st == CYW43_LINK_JOIN || st == CYW43_LINK_NOIP) return;      /* en cours */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - bg_last_try < BG_RETRY_MS) return;
    bg_last_try = now;
    cyw43_arch_wifi_connect_async(modem->cfg.ssid, modem->cfg.pass[0] ? modem->cfg.pass : NULL, auth_for(modem->cfg.pass));
}

static int wifi_join(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    uint32_t auth = auth_for(pass);
    bg_enabled = false;                      /* pas de tentative concurrente pendant la commande */
    if (cyw43_arch_wifi_connect_async(ssid, pass[0] ? pass : NULL, auth) != 0) return AT_NET_FAIL;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    bool seen_nonet = false;
    while (to_ms_since_boot(get_absolute_time()) - t0 < 30000) {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP) {
            strncpy(joined_ssid, ssid, AT_SSID_MAX);
            apply_ip_config();
            bg_was_up = true;
            net_pico_background_join(true);  /* reconnexion automatique si le lien tombe */
            return AT_NET_OK;
        }
        if (st == CYW43_LINK_BADAUTH) return AT_NET_BAD_PASSWORD;
        if (st == CYW43_LINK_FAIL) return AT_NET_FAIL;
        if (st == CYW43_LINK_NONET) {
            /* comme cyw43_arch_wifi_connect_until : le réseau n'est pas encore
               vu (fréquent juste après le boot) → on relance jusqu'au délai */
            seen_nonet = true;
            if (cyw43_arch_wifi_connect_async(ssid, pass[0] ? pass : NULL, auth) != 0) return AT_NET_FAIL;
        }
        wait_ms(100);
    }
    return seen_nonet ? AT_NET_NO_AP : AT_NET_TIMEOUT;
}

static void wifi_leave(void *ctx)
{
    (void)ctx;
    bg_enabled = false;
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
        wait_ms(50);
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
    uint8_t bssid[6] = { 0 };
    cyw43_wifi_get_bssid(&cyw43_state, bssid);
    snprintf(info->bssid, sizeof info->bssid, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    strcpy(info->ssid, joined_ssid);
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    info->rssi = rssi;
    uint32_t ch = 0;
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof ch, (uint8_t *)&ch, CYW43_ITF_STA) == 0)
        info->channel = (int)ch;
    info->dhcp = modem->cfg.dhcp;
}

/* -------------------------------------------------------------- DNS */

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
    while (!dns_done && to_ms_since_boot(get_absolute_time()) - t0 < CONNECT_TIMEOUT_MS) wait_ms(1);
    if (!dns_done || ip_addr_isany(&dns_result)) return false;
    *out = dns_result;
    return true;
}

/* --------------------------------------------------------- TCP / TLS */
/*
 * Une seule API (altcp) pour le TCP en clair et le TLS. En TLS : config
 * client mbedTLS sans chaîne de CA chargée : les racines (certs/roots.pem →
 * roots_store, en flash) sont cherchées à la demande par roots_ca_cb, et seule
 * la racine utile est décodée en RAM (US-T13) ; vérification obligatoire de
 * la chaîne et du nom (SNI), dates vérifiées
 * dans tls_verify_cb (heure SNTP exigée), ticket de session réutilisé pour
 * le même hôte:port (prophet.neo ouvre une connexion par bloc « Range »).
 */

static struct altcp_tls_config *tls_conf;
static bool tls_conf_verify_set;
static struct altcp_tls_session tls_session;
static bool tls_session_valid;
static char tls_session_host[AT_HOST_MAX + 8];
static volatile int tls_last_err;        /* dernier code mbedTLS (0 = aucun) */
static uint32_t connect_t0;
static volatile int last_lwip_err;       /* dernier err_t reçu (on_err / on_connected) */
static volatile uint32_t last_err_ms;
static char tls_info_buf[860];
static volatile int tls_root_idx = -1;   /* racine du magasin retenue au dernier handshake */
static uint32_t tls_handshake_ms;        /* durée du dernier handshake       */
static const char *tls_suite = "";       /* suite négociée                   */
static bool tls_resumed;

/* mbedtls_time() → time() → _gettimeofday (newlib, remplace la version faible
   du SDK) : heure SNTP, sans verrou (utilisable dans le contexte lwIP). */
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) return -1;
    if (!sntp_epoch) { tv->tv_sec = 0; tv->tv_usec = 0; return 0; }
    uint32_t d = to_ms_since_boot(get_absolute_time()) - sntp_at_ms;
    tv->tv_sec = sntp_epoch + d / 1000;
    tv->tv_usec = (suseconds_t)((d % 1000) * 1000);
    return 0;
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT : temps monotone en ms (tickets, délais). */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)to_ms_since_boot(get_absolute_time());
}

/* Rappel mbedTLS par certificat de la chaîne : ajoute EXPIRED / FUTURE selon
   l'heure SNTP (mbedTLS ne le fait pas sans MBEDTLS_HAVE_TIME_DATE). */
static volatile uint32_t tls_verify_flags[4];
static volatile uint32_t tls_verify_ms[4];   /* instant (ms après connect) de chaque vérification */
static volatile int tls_verify_depths;

static int tls_verify_cb(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx;
    struct timeval tv;
    _gettimeofday(&tv, NULL);
    if (tv.tv_sec == 0) { *flags |= MBEDTLS_X509_BADCERT_OTHER; return 0; }
    int now6[6];
    civil_from_epoch(tv.tv_sec, now6);
    int from[6] = { crt->valid_from.year, crt->valid_from.mon, crt->valid_from.day,
                    crt->valid_from.hour, crt->valid_from.min, crt->valid_from.sec };
    int to[6]   = { crt->valid_to.year, crt->valid_to.mon, crt->valid_to.day,
                    crt->valid_to.hour, crt->valid_to.min, crt->valid_to.sec };
    if (cmp6(now6, from) < 0) *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if (cmp6(now6, to) > 0)   *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    int r = roots_index_of(&roots_store, crt->raw.p);   /* racine du magasin ? */
    if (r >= 0) tls_root_idx = r;
    if (depth >= 0 && depth < 4) {
        tls_verify_flags[depth] = *flags;
        tls_verify_ms[depth] = to_ms_since_boot(get_absolute_time()) - connect_t0;
        if (depth + 1 > tls_verify_depths) tls_verify_depths = depth + 1;
    }
    return 0;
}

/* Journal mbedTLS : on ne garde que les alertes et les échecs (net_pico_diag). */
static void tls_dbg(void *ctx, int level, const char *file, int line, const char *str)
{
    (void)ctx; (void)file; (void)line;
    if (level <= 1 || strstr(str, "alert") || strstr(str, "fail"))
        net_pico_diag("mbedtls: %s", str);
}

static bool tls_init(void)
{
    if (tls_conf) return true;
    cyw43_arch_lwip_begin();
    tls_conf = altcp_tls_create_config_client(NULL, 0);   /* CA : roots_ca_cb ; → VERIFY_REQUIRED */
    cyw43_arch_lwip_end();
    altcp_tls_init_session(&tls_session);
    return tls_conf != NULL;
}

/* Tas newlib : arena = plus haut niveau atteint par sbrk (ne redescend pas),
   donc pic de consommation depuis le démarrage ; limite = fin de la RAM. */
static void heap_stats(unsigned *in_use, unsigned *peak, unsigned *max)
{
    extern char end, __StackLimit;
    struct mallinfo mi = mallinfo();
    *in_use = (unsigned)mi.uordblks;
    *peak = (unsigned)mi.arena;
    *max = (unsigned)(&__StackLimit - &end);
}

static const char *tls_info_op(void *ctx)
{
    (void)ctx;
    unsigned used, peak, max;
    heap_stats(&used, &peak, &max);
    int ri = tls_root_idx;
    int n = snprintf(tls_info_buf, sizeof tls_info_buf,
             "TLS: mbedTLS " MBEDTLS_VERSION_STRING ", TLS 1.2 client, verify CA+SNI+dates, roots: %d in flash (on demand), "
             "last root: %s, heap: %u used, %u peak, %u max, "
             "time: %s, last handshake: %lu ms (%s%s), last err: %d at %lu ms, verify:",
             roots_store.count, ri >= 0 ? roots_store.e[ri].name : "-", used, peak, max,
             sntp_epoch ? "synced" : "NONE", (unsigned long)tls_handshake_ms,
             tls_suite, tls_resumed ? ", resumed" : "", last_lwip_err, (unsigned long)last_err_ms);
    for (int i = 0; i < tls_verify_depths && n < (int)sizeof tls_info_buf - 12; i++)
        n += snprintf(tls_info_buf + n, sizeof tls_info_buf - n, " d%d=0x%lx@%lums", i, (unsigned long)tls_verify_flags[i], (unsigned long)tls_verify_ms[i]);
    for (unsigned i = 0; i < 6 && n < (int)sizeof tls_info_buf - 4; i++) {
        unsigned k = (diag_n + i) % 6;
        if (diag_buf[k][0]) n += snprintf(tls_info_buf + n, sizeof tls_info_buf - n, "\r\nlwip: %s", diag_buf[k]);
    }
    return tls_info_buf;
}

static void handshake_guard(bool on);

static const char *tls_selftest_op(void *ctx)
{
    (void)ctx;
    static char buf[160];
    watchdog_update();
    int aes = mbedtls_aes_self_test(0);   watchdog_update();
    int gcm = mbedtls_gcm_self_test(0);   watchdog_update();
    int s256 = mbedtls_sha256_self_test(0); watchdog_update();
    int s512 = mbedtls_sha512_self_test(0); watchdog_update();
    int drbg = mbedtls_ctr_drbg_self_test(0); watchdog_update();
    handshake_guard(true);                /* ecp : plusieurs secondes */
    int ecp = mbedtls_ecp_self_test(0);
    handshake_guard(false);
    watchdog_update();
    int mpi = mbedtls_mpi_self_test(0);
    snprintf(buf, sizeof buf, "selftest aes=%d gcm=%d sha256=%d sha512=%d ctr_drbg=%d ecp=%d mpi=%d (0 = OK)",
             aes, gcm, s256, s512, drbg, ecp, mpi);
    return buf;
}

static struct altcp_pcb *pcb;            /* connexion active                  */
static struct altcp_pcb *listen_pcb;     /* écoute                            */
static struct altcp_pcb *pending_pcb;    /* appel entrant non répondu         */
static volatile int connect_state;       /* 0 en cours, 1 ok, <0 erreur       */
static bool pcb_is_tls;

/* Le handshake TLS (ECDSA/ECDHE) s'exécute dans le contexte lwIP (IRQ de
   basse priorité) et peut occuper le CPU plus de 8 s d'affilée sur RP2040,
   au-delà du maximum du watchdog. Pendant un handshake, ce timer (IRQ
   prioritaire) rafraîchit le watchdog, au plus TLS_HANDSHAKE_MAX_S secondes. */
#define TLS_HANDSHAKE_MAX_S 60
static repeating_timer_t handshake_timer;
static volatile int handshake_guard_s;

static bool handshake_tick(repeating_timer_t *t)
{
    (void)t;
    if (handshake_guard_s > 0) { handshake_guard_s--; watchdog_update(); }
    return true;
}

static void handshake_guard(bool on)
{
    static bool timer_started;
    if (on && !timer_started) {
        add_repeating_timer_ms(1000, handshake_tick, NULL, &handshake_timer);
        timer_started = true;
    }
    handshake_guard_s = on ? TLS_HANDSHAKE_MAX_S : 0;
}

static void pcb_detach(struct altcp_pcb *p)
{
    altcp_arg(p, NULL);
    altcp_recv(p, NULL);
    altcp_err(p, NULL);
    altcp_sent(p, NULL);
    if (altcp_close(p) != ERR_OK) altcp_abort(p);
}

static err_t on_recv(void *arg, struct altcp_pcb *p, struct pbuf *buf, err_t err)
{
    (void)arg; (void)err;
    if (!buf) {                                   /* fermeture distante */
        at_modem_remote_closed(modem);
        if (p == pcb) { pcb_detach(p); pcb = NULL; }
        return ERR_OK;
    }
    if (p == pending_pcb) return ERR_MEM;                          /* appel non décroché : lwIP garde les données */
    if (at_modem_rx_space(modem) < buf->tot_len) return ERR_MEM; /* lwIP réessaie */
    for (struct pbuf *q = buf; q; q = q->next) at_modem_rx_push(modem, q->payload, q->len);
    altcp_recved(p, buf->tot_len);
    pbuf_free(buf);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg;
    last_lwip_err = err; last_err_ms = to_ms_since_boot(get_absolute_time()) - connect_t0;
    /* le pcb est déjà libéré par lwIP / altcp */
    if (connect_state == 0) connect_state = (err == ERR_CLSD || err == ERR_RST || err == ERR_ABRT) ? -2 : -1;
    pcb = NULL;
    at_modem_remote_closed(modem);
}

static err_t on_connected(void *arg, struct altcp_pcb *p, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { last_lwip_err = 100 + err; last_err_ms = to_ms_since_boot(get_absolute_time()) - connect_t0; }
    if (err == ERR_OK && pcb_is_tls) {
        tls_handshake_ms = to_ms_since_boot(get_absolute_time()) - connect_t0;
        mbedtls_ssl_context *ssl = altcp_tls_context(p);
        tls_suite = mbedtls_ssl_get_ciphersuite(ssl);
        tls_resumed = (tls_verify_depths == 0);
        /* ticket/identifiant de session pour la prochaine connexion au même hôte */
        if (altcp_tls_get_session(p, &tls_session) == ERR_OK) tls_session_valid = true;
    }
    connect_state = (err == ERR_OK) ? 1 : -1;
    return ERR_OK;
}

static int tcp_connect_op(void *ctx, const char *host, uint16_t port, bool tls)
{
    (void)ctx;
    ip_addr_t addr;
    net_pico_stage(11);
    if (tls) {
        if (!sntp_epoch) return AT_NET_NO_TIME;           /* certificats non vérifiables */
        if (!tls_init()) return AT_NET_NO_TLS;
    }
    if (!resolve(host, &addr)) return AT_NET_DNS_FAIL;
    net_pico_stage(12);
    char hostport[sizeof tls_session_host];
    snprintf(hostport, sizeof hostport, "%s:%u", host, port);
    tls_last_err = 0;
    tls_verify_depths = 0;
    tls_root_idx = -1;
    cyw43_arch_lwip_begin();
    struct altcp_pcb *p = tls ? altcp_tls_new(tls_conf, IP_GET_TYPE(&addr))
                              : altcp_tcp_new_ip_type(IP_GET_TYPE(&addr));
    if (!p) { cyw43_arch_lwip_end(); return AT_NET_FAIL; }
    if (tls) {
        mbedtls_ssl_context *ssl = altcp_tls_context(p);
        mbedtls_ssl_set_hostname(ssl, host);                       /* SNI + vérification du nom */
        if (!tls_conf_verify_set) {
            /* ceinture et bretelles avec ALTCP_MBEDTLS_AUTHMODE : jamais OPTIONAL */
            mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)ssl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_verify((mbedtls_ssl_config *)ssl->conf, tls_verify_cb, NULL);
            mbedtls_ssl_conf_ca_cb((mbedtls_ssl_config *)ssl->conf, roots_ca_cb, (void *)&roots_store);
            mbedtls_ssl_conf_dbg((mbedtls_ssl_config *)ssl->conf, tls_dbg, NULL);
            mbedtls_debug_set_threshold(2);
            tls_conf_verify_set = true;
        }
        if (tls_session_valid && !strcmp(tls_session_host, hostport))
            altcp_tls_set_session(p, &tls_session);
        else
            tls_session_valid = false;
        strcpy(tls_session_host, hostport);
    }
    altcp_recv(p, on_recv);
    altcp_err(p, on_err);
    connect_state = 0;
    pcb = p;
    pcb_is_tls = tls;
    connect_t0 = to_ms_since_boot(get_absolute_time());
    if (tls) handshake_guard(true);
    err_t e = altcp_connect(p, &addr, port, on_connected);
    cyw43_arch_lwip_end();
    net_pico_stage(13);
    if (e != ERR_OK) { cyw43_arch_lwip_begin(); pcb_detach(p); cyw43_arch_lwip_end(); pcb = NULL; return AT_NET_CONNECT_FAIL; }
    uint32_t timeout = tls ? TLS_CONNECT_TIMEOUT_MS : CONNECT_TIMEOUT_MS;
    while (connect_state == 0 && to_ms_since_boot(get_absolute_time()) - connect_t0 < timeout) wait_ms(1);
    handshake_guard(false);
    net_pico_stage(14);
    if (connect_state != 1) {
        net_pico_stage(15);
        int st = connect_state;
        cyw43_arch_lwip_begin();
        if (pcb) pcb_detach(pcb);
        cyw43_arch_lwip_end();
        pcb = NULL;
        modem->remote_closed = false;
        if (tls) { tls_session_valid = false; return (st == -2 || st == -1) ? AT_NET_TLS_FAIL : AT_NET_CONNECT_FAIL; }
        return AT_NET_CONNECT_FAIL;
    }
    net_pico_stage(16);
    return AT_NET_OK;
}

static int tcp_send_op(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (len) {
        if (!pcb) return AT_NET_FAIL;
        cyw43_arch_lwip_begin();
        size_t chunk = altcp_sndbuf(pcb);
        if (chunk > len) chunk = len;
        err_t e = ERR_OK;
        if (chunk) {
            e = altcp_write(pcb, data, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) altcp_output(pcb);
        }
        cyw43_arch_lwip_end();
        if (e == ERR_OK && chunk) { data += chunk; len -= chunk; t0 = to_ms_since_boot(get_absolute_time()); continue; }
        if (e != ERR_OK && e != ERR_MEM) return AT_NET_FAIL;
        if (to_ms_since_boot(get_absolute_time()) - t0 > SEND_TIMEOUT_MS) return AT_NET_TIMEOUT;
        wait_ms(1);
    }
    return AT_NET_OK;
}

static void tcp_close_op(void *ctx)
{
    (void)ctx;
    cyw43_arch_lwip_begin();
    if (pcb) { pcb_detach(pcb); pcb = NULL; }
    cyw43_arch_lwip_end();
}

static bool tcp_connected_op(void *ctx)
{
    (void)ctx;
    return pcb != NULL && connect_state == 1;
}

static err_t on_accept(void *arg, struct altcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    if (pcb || pending_pcb) { altcp_abort(newpcb); return ERR_ABRT; } /* occupé */
    pending_pcb = newpcb;
    altcp_recv(newpcb, on_recv);
    altcp_err(newpcb, on_err);
    last_ring_ms = to_ms_since_boot(get_absolute_time());
    at_modem_ring(modem);
    return ERR_OK;
}

static int tcp_listen_op(void *ctx, uint16_t port)
{
    (void)ctx;
    cyw43_arch_lwip_begin();
    if (listen_pcb) { altcp_close(listen_pcb); listen_pcb = NULL; }
    int r = AT_NET_OK;
    if (port) {
        struct altcp_pcb *p = altcp_tcp_new_ip_type(IPADDR_TYPE_ANY);
        if (!p || altcp_bind(p, IP_ANY_TYPE, port) != ERR_OK) { if (p) altcp_close(p); r = AT_NET_FAIL; }
        else {
            listen_pcb = altcp_listen_with_backlog(p, 1);
            if (!listen_pcb) r = AT_NET_FAIL; else altcp_accept(listen_pcb, on_accept);
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
    if (ok) { pcb = pending_pcb; pending_pcb = NULL; connect_state = 1; pcb_is_tls = false; modem->remote_closed = false; }
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
        while (ping_reply_ms < 0 && to_ms_since_boot(get_absolute_time()) - ping_sent_ms < 3000) wait_ms(1);
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
    .reset = reset_op, .bootsel = bootsel_op, .version = version_op, .tls_info = tls_info_op, .tls_selftest = tls_selftest_op,
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
    bg_poll();
    /* sonnerie répétée tant que l'appel entrant n'est pas décroché */
    if (pending_pcb && to_ms_since_boot(get_absolute_time()) - last_ring_ms >= RING_PERIOD_MS) {
        last_ring_ms = to_ms_since_boot(get_absolute_time());
        at_modem_ring(modem);
    }
}
