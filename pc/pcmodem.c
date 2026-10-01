/*
 * pcmodem.c — le modem Neo6502picowifi simulé sur PC (Linux).
 *
 * Le cœur portable (src/at_modem.c, http_parse.c, tnfs_link.c) tourne tel
 * quel ; ses opérations réseau passent par les sockets du PC. Le modem est
 * exposé comme un port série virtuel (pseudo-terminal) : validate.py, minicom,
 * un émulateur Neo6502 (reload, Phosphoneo…) s'y connectent comme au Pico W.
 *
 *  ./pcmodem [-l lien] [-T lien_tnfs] [-c fichier_config]
 *    -l  lien symbolique vers le port AT (ex. /tmp/neomodem)
 *    -T  second port virtuel « TNFS » (trames longueur 2 o + datagramme)
 *    -c  configuration persistante (défaut : ./pcmodem.cfg)
 *
 * TLS : mbedTLS du SDK avec la configuration du firmware et le même magasin
 * de racines (roots_store, roots_ca_cb) ; compilé sans TLS si le SDK manque
 * (PCMODEM_NO_TLS : "SSL" → no TLS). Dates des certificats vérifiées par
 * mbedTLS avec l'horloge du PC (pas de SNTP exigé).
 * Différences avec le Pico W : pas de point d'accès, Wi-Fi simulé (le réseau
 * du PC ; AT+CWJAP accepte tout SSID), AT+CWLAP renvoie un réseau fictif,
 * AT+PING passe par la commande ping, pas de reprise de session TLS.
 */
#define _GNU_SOURCE
#include "../src/at_modem.h"
#include "../src/tnfs_link.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#ifndef PCMODEM_NO_TLS
#include "mbedtls/build_info.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"          /* codes MBEDTLS_ERR_NET_* seulement */
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "../src/roots_ca_cb.h"
#endif

#ifndef PCMODEM_VERSION
#define PCMODEM_VERSION "0.0.0"
#endif

static struct at_modem modem;
static int pty_at = -1, pty_at_slave = -1;
static int pty_tnfs = -1, pty_tnfs_slave = -1;
static int link_fd = -1;                 /* lien sortant TCP ou UDP */
static bool link_is_udp;
static int listen_fd = -1, pending_fd = -1;
static int tnfs_fd = -1;                 /* UDP du port TNFS */
static char tnfs_cur[AT_HOST_MAX + 8];
static bool wifi_up = true;
static char joined[AT_SSID_MAX + 1] = "pc-network";
static const char *cfg_path = "pcmodem.cfg";
static struct tnfs_rx tnfs_rx;

/* ----------------------------------------------------------- utilitaires */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void write_all(int fd, const uint8_t *d, size_t n)
{
    while (n) {
        ssize_t k = write(fd, d, n);
        if (k < 0) { if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; } return; }
        d += k; n -= (size_t)k;
    }
}

/* pseudo-terminal brut ; l'esclave reste ouvert pour que le maître ne voie
   jamais EIO quand aucun client n'est connecté */
static int open_pty(int *slave, const char *link)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty"); exit(1); }
    const char *name = ptsname(m);
    *slave = open(name, O_RDWR | O_NOCTTY);
    struct termios t;
    tcgetattr(*slave, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);
    tcsetattr(*slave, TCSANOW, &t);
    fcntl(m, F_SETFL, O_NONBLOCK);
    if (link) {
        unlink(link);
        if (symlink(name, link)) perror("symlink");
    }
    printf("%s -> %s\n", link ? link : "port", name);
    fflush(stdout);
    return m;
}

static bool resolve(const char *host, uint16_t port, int type, struct sockaddr_storage *sa, socklen_t *len)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = type }, *res;
    char p[8];
    snprintf(p, sizeof p, "%u", port);
    if (getaddrinfo(host, p, &hints, &res)) return false;
    memcpy(sa, res->ai_addr, res->ai_addrlen);
    *len = res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

/* -------------------------------------------------------------- opérations */

static void op_write(void *c, const uint8_t *d, size_t n) { (void)c; write_all(pty_at, d, n); }
static uint32_t op_millis(void *c) { (void)c; return now_ms(); }

static int op_join(void *c, const char *ssid, const char *pass)
{
    (void)c; (void)pass;
    snprintf(joined, sizeof joined, "%s", ssid);
    wifi_up = true;
    return AT_NET_OK;
}
static void op_leave(void *c) { (void)c; wifi_up = false; }
static bool op_wifi(void *c) { (void)c; return wifi_up; }
static int op_scan(void *c, at_scan_cb cb, void *x)
{
    (void)c;
    cb(x, 3, joined, -40);
    return AT_NET_OK;
}

static void op_ip_info(void *c, struct at_ip_info *i)
{
    (void)c;
    memset(i, 0, sizeof *i);
    strcpy(i->ip, "0.0.0.0");
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9) };
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);                 /* aucun paquet n'est envoyé */
    if (u >= 0 && connect(u, (struct sockaddr *)&a, sizeof a) == 0) {
        socklen_t l = sizeof a;
        getsockname(u, (struct sockaddr *)&a, &l);
        inet_ntop(AF_INET, &a.sin_addr, i->ip, sizeof i->ip);
    }
    if (u >= 0) close(u);
    strcpy(i->gateway, "0.0.0.0");
    strcpy(i->netmask, "255.255.255.0");
    strcpy(i->dns, "0.0.0.0");
    strcpy(i->mac, "02:00:5e:65:02:01");
    strcpy(i->bssid, "02:00:5e:00:00:01");
    snprintf(i->ssid, sizeof i->ssid, "%s", joined);
    i->channel = 6;
    i->rssi = -40;
    i->dhcp = true;
}

/* ------------------------------------------------------------------- TLS */

#ifndef PCMODEM_NO_TLS
static mbedtls_ssl_config tls_conf;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_entropy_context entropy;
static mbedtls_ssl_context ssl;
static bool tls_ready, tls_on;
static char tls_info_buf[256];

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    (void)ctx;
    ssize_t k = send(link_fd, b, n, MSG_NOSIGNAL);
    if (k >= 0) return (int)k;
    return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    (void)ctx;
    ssize_t k = recv(link_fd, b, n, 0);
    if (k > 0) return (int)k;
    if (k == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    return errno == EAGAIN ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
}

static bool tls_init(void)
{
    if (tls_ready) return true;
    mbedtls_ssl_config_init(&tls_conf);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"pcmodem", 7)
        || mbedtls_ssl_config_defaults(&tls_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT)) return false;
    mbedtls_ssl_conf_authmode(&tls_conf, MBEDTLS_SSL_VERIFY_REQUIRED);   /* comme le firmware */
    mbedtls_ssl_conf_rng(&tls_conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_ca_cb(&tls_conf, roots_ca_cb, (void *)&roots_store);
    tls_ready = true;
    return true;
}

static int tls_handshake(const char *host)
{
    if (!tls_init()) return AT_NET_NO_TLS;
    mbedtls_ssl_init(&ssl);
    if (mbedtls_ssl_setup(&ssl, &tls_conf) || mbedtls_ssl_set_hostname(&ssl, host)) {
        mbedtls_ssl_free(&ssl);
        return AT_NET_TLS_FAIL;
    }
    mbedtls_ssl_set_bio(&ssl, NULL, bio_send, bio_recv, NULL);
    struct timeval tv = { 15, 0 };
    setsockopt(link_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int r;
    while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    if (r) {
        char e[100];
        mbedtls_strerror(r, e, sizeof e);
        snprintf(tls_info_buf, sizeof tls_info_buf, "TLS: %s (PC), %d roots, last error: -0x%04x %s, verify: 0x%x",
                 MBEDTLS_VERSION_STRING_FULL, roots_store.count, -r, e, mbedtls_ssl_get_verify_result(&ssl));
        mbedtls_ssl_free(&ssl);
        return AT_NET_TLS_FAIL;
    }
    snprintf(tls_info_buf, sizeof tls_info_buf, "TLS: %s (PC), %d roots, last handshake: %s",
             MBEDTLS_VERSION_STRING_FULL, roots_store.count, mbedtls_ssl_get_ciphersuite(&ssl));
    tls_on = true;
    return AT_NET_OK;
}

static const char *op_tls_info(void *c)
{
    (void)c;
    if (!tls_info_buf[0])
        snprintf(tls_info_buf, sizeof tls_info_buf, "TLS: %s (PC), %d roots", MBEDTLS_VERSION_STRING_FULL, roots_store.count);
    return tls_info_buf;
}
#endif

static void link_close(void)
{
#ifndef PCMODEM_NO_TLS
    if (tls_on) { mbedtls_ssl_close_notify(&ssl); mbedtls_ssl_free(&ssl); tls_on = false; }
#endif
    if (link_fd >= 0) close(link_fd);
    link_fd = -1;
}

static int op_tcp_connect(void *c, const char *host, uint16_t port, bool tls)
{
    (void)c;
#ifdef PCMODEM_NO_TLS
    if (tls) return AT_NET_NO_TLS;
#endif
    struct sockaddr_storage sa;
    socklen_t len;
    if (!resolve(host, port, SOCK_STREAM, &sa, &len)) return AT_NET_DNS_FAIL;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);       /* connect borné à 10 s */
    if (connect(fd, (struct sockaddr *)&sa, len)) { close(fd); return AT_NET_CONNECT_FAIL; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    link_fd = fd;
    link_is_udp = false;
#ifndef PCMODEM_NO_TLS
    if (tls) {
        int r = tls_handshake(host);
        if (r != AT_NET_OK) { close(link_fd); link_fd = -1; return r; }
    }
#endif
    return AT_NET_OK;
}

static int op_udp_connect(void *c, const char *host, uint16_t port)
{
    (void)c;
    struct sockaddr_storage sa;
    socklen_t len;
    if (!resolve(host, port, SOCK_DGRAM, &sa, &len)) return AT_NET_DNS_FAIL;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (connect(fd, (struct sockaddr *)&sa, len)) { close(fd); return AT_NET_FAIL; }
    link_fd = fd;
    link_is_udp = true;
    return AT_NET_OK;
}

static int op_send(void *c, const uint8_t *d, size_t n)
{
    (void)c;
    if (link_fd < 0) return AT_NET_FAIL;
    if (link_is_udp) return send(link_fd, d, n, 0) == (ssize_t)n ? AT_NET_OK : AT_NET_FAIL;
#ifndef PCMODEM_NO_TLS
    if (tls_on) {
        while (n) {
            int k = mbedtls_ssl_write(&ssl, d, n);
            if (k == MBEDTLS_ERR_SSL_WANT_WRITE || k == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (k <= 0) return AT_NET_FAIL;
            d += k; n -= (size_t)k;
        }
        return AT_NET_OK;
    }
#endif
    while (n) {
        ssize_t k = send(link_fd, d, n, MSG_NOSIGNAL);
        if (k <= 0) return AT_NET_FAIL;
        d += k; n -= (size_t)k;
    }
    return AT_NET_OK;
}

static void op_close(void *c) { (void)c; link_close(); }
static bool op_connected(void *c) { (void)c; return link_fd >= 0; }

static int op_listen(void *c, uint16_t port)
{
    (void)c;
    if (listen_fd >= 0) close(listen_fd);
    listen_fd = -1;
    if (!port) return AT_NET_OK;
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 1)) { close(fd); return AT_NET_FAIL; }
    listen_fd = fd;
    return AT_NET_OK;
}

static bool op_accept(void *c)
{
    (void)c;
    if (pending_fd < 0) return false;
    link_fd = pending_fd;
    pending_fd = -1;
    link_is_udp = false;
    return true;
}

static void op_save(void *c, const struct at_config *cfg)
{
    (void)c;
    FILE *f = fopen(cfg_path, "wb");
    if (f) { fwrite(cfg, sizeof *cfg, 1, f); fclose(f); }
}

static void op_sntp(void *c, char *o, size_t n)
{
    (void)c;
    time_t t = time(NULL) + (time_t)modem.cfg.sntp_tz * 3600;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(o, n, "%a %b %d %H:%M:%S %Y", &tm);
}

static int op_ping(void *c, const char *host)
{
    (void)c;
    for (const char *p = host; *p; p++)                             /* nom déjà filtré, prudence */
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-')) return -1;
    char cmdline[128], line[256];
    snprintf(cmdline, sizeof cmdline, "ping -c1 -W2 %s 2>/dev/null", host);
    FILE *f = popen(cmdline, "r");
    if (!f) return -1;
    int ms = -1;
    while (fgets(line, sizeof line, f)) {
        char *t = strstr(line, "time=");
        if (t) { ms = (int)(atof(t + 5) + 0.5); if (ms < 1) ms = 1; }
    }
    pclose(f);
    return ms;
}

static void load_config(struct at_config *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    FILE *f = fopen(cfg_path, "rb");
    if (f) { if (fread(cfg, sizeof *cfg, 1, f) != 1) cfg->magic = 0; fclose(f); }
}

static const struct at_modem_ops ops;

static void op_reset(void *c)
{
    (void)c;
    link_close();
    if (pending_fd >= 0) close(pending_fd);
    pending_fd = -1;
    struct at_config cfg;
    load_config(&cfg);
    at_modem_init(&modem, &ops, &cfg);
    op_listen(NULL, modem.cfg.hosts_enforce ? 0 : modem.cfg.listen_port);
    wifi_up = true;
    const char ready[] = "\r\nready (AT+RST)\r\n";
    write_all(pty_at, (const uint8_t *)ready, sizeof ready - 1);
}

static const char *op_version(void *c) { (void)c; return PCMODEM_VERSION; }
static const char *op_build(void *c) { (void)c; return "pcmodem (simulation PC)"; }
static void pump(int timeout_ms);
static void op_idle(void *c) { (void)c; pump(1); }

static const struct at_modem_ops ops = {
    .write = op_write, .millis = op_millis,
    .wifi_join = op_join, .wifi_leave = op_leave, .wifi_connected = op_wifi,
    .wifi_scan = op_scan, .ip_info = op_ip_info,
    .tcp_connect = op_tcp_connect, .tcp_send = op_send, .tcp_close = op_close,
    .tcp_connected = op_connected, .tcp_listen = op_listen, .tcp_accept = op_accept,
    .config_save = op_save, .sntp_time = op_sntp, .ping = op_ping, .reset = op_reset,
    .version = op_version, .build = op_build,
    .udp_connect = op_udp_connect, .idle = op_idle,
#ifndef PCMODEM_NO_TLS
    .tls_info = op_tls_info,
#endif
};

/* --------------------------------------------------------------- TNFS (-T) */

static void tnfs_frame(void *ctx, const uint8_t *d, size_t len)
{
    (void)ctx;
    const struct at_config *cfg = &modem.cfg;
    if (!cfg->tnfs_host[0] || !wifi_up) return;
    if (!at_modem_host_allowed(cfg, cfg->tnfs_host)) return;
    char key[sizeof tnfs_cur];
    snprintf(key, sizeof key, "%s:%u", cfg->tnfs_host, cfg->tnfs_port);
    if (tnfs_fd < 0 || strcmp(key, tnfs_cur)) {                     /* AT$TNFS a changé */
        if (tnfs_fd >= 0) close(tnfs_fd);
        tnfs_fd = -1;
        struct sockaddr_storage sa;
        socklen_t sl;
        if (!resolve(cfg->tnfs_host, cfg->tnfs_port, SOCK_DGRAM, &sa, &sl)) return;
        tnfs_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (connect(tnfs_fd, (struct sockaddr *)&sa, sl)) { close(tnfs_fd); tnfs_fd = -1; return; }
        strcpy(tnfs_cur, key);
    }
    send(tnfs_fd, d, len, 0);
}

/* ------------------------------------------------------------------ boucle */

/* Fait avancer le réseau : données entrantes vers le tampon du modem (sans
   le dépasser : TCP attend, UDP perd comme sur le Pico), appels entrants,
   port TNFS. timeout_ms : attente maximale. */
static void pump(int timeout_ms)
{
    struct pollfd p[5];
    int n = 0, i_link = -1, i_listen = -1, i_tnfs_net = -1;
    if (link_fd >= 0 && (link_is_udp || at_modem_rx_space(&modem) > 0)) { i_link = n; p[n++] = (struct pollfd){ link_fd, POLLIN, 0 }; }
    if (listen_fd >= 0 && pending_fd < 0) { i_listen = n; p[n++] = (struct pollfd){ listen_fd, POLLIN, 0 }; }
    if (tnfs_fd >= 0) { i_tnfs_net = n; p[n++] = (struct pollfd){ tnfs_fd, POLLIN, 0 }; }
    bool tls_pending = false;
#ifndef PCMODEM_NO_TLS
    tls_pending = tls_on && link_fd >= 0 && mbedtls_ssl_get_bytes_avail(&ssl) > 0 && at_modem_rx_space(&modem) > 0;
#endif
    if (poll(p, (nfds_t)n, tls_pending ? 0 : timeout_ms) <= 0 && !tls_pending) return;
    uint8_t buf[2048];
    if (i_link >= 0 && (tls_pending || (p[i_link].revents & (POLLIN | POLLHUP | POLLERR)))) {
#ifndef PCMODEM_NO_TLS
        if (tls_on) {
            size_t room = at_modem_rx_space(&modem);
            int k = mbedtls_ssl_read(&ssl, buf, room < sizeof buf ? room : sizeof buf);
            if (k > 0) at_modem_rx_push(&modem, buf, (size_t)k);
            else if (k != MBEDTLS_ERR_SSL_WANT_READ && k != MBEDTLS_ERR_SSL_WANT_WRITE && k != MBEDTLS_ERR_SSL_TIMEOUT) {
                link_close();                                        /* close_notify, fin ou erreur */
                at_modem_remote_closed(&modem);
            }
        } else
#endif
        if (link_is_udp) {
            ssize_t k = recv(link_fd, buf, sizeof buf, 0);
            if (k > 0) at_modem_rx_push_dgram(&modem, buf, (size_t)k);
        } else {
            size_t room = at_modem_rx_space(&modem);
            ssize_t k = recv(link_fd, buf, room < sizeof buf ? room : sizeof buf, 0);
            if (k > 0) at_modem_rx_push(&modem, buf, (size_t)k);
            else if (k == 0 || (errno != EAGAIN && errno != EINTR)) { link_close(); at_modem_remote_closed(&modem); }
        }
    }
    if (i_listen >= 0 && (p[i_listen].revents & POLLIN)) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd >= 0) {
            if (link_fd >= 0) close(fd);                             /* occupé, comme le Pico */
            else { pending_fd = fd; at_modem_ring(&modem); }
        }
    }
    if (i_tnfs_net >= 0 && (p[i_tnfs_net].revents & POLLIN)) {
        ssize_t k = recv(tnfs_fd, buf, sizeof buf, 0);
        if (k > 0 && k <= TNFS_DGRAM_MAX && pty_tnfs >= 0) {
            uint8_t hdr[2] = { (uint8_t)k, (uint8_t)(k >> 8) };
            write_all(pty_tnfs, hdr, 2);
            write_all(pty_tnfs, buf, (size_t)k);
        }
    }
}

int main(int argc, char **argv)
{
    const char *link = NULL, *tnfs_link_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "l:T:c:h")) != -1) {
        if (opt == 'l') link = optarg;
        else if (opt == 'T') tnfs_link_path = optarg;
        else if (opt == 'c') cfg_path = optarg;
        else { fprintf(stderr, "usage : %s [-l lien] [-T lien_tnfs] [-c config]\n", argv[0]); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    pty_at = open_pty(&pty_at_slave, link);
    if (tnfs_link_path) pty_tnfs = open_pty(&pty_tnfs_slave, tnfs_link_path);
    tnfs_rx_init(&tnfs_rx);

    struct at_config cfg;
    load_config(&cfg);
    at_modem_init(&modem, &ops, &cfg);
    op_listen(NULL, modem.cfg.hosts_enforce ? 0 : modem.cfg.listen_port);
    const char ready[] = "\r\nready (power-on)\r\n";
    write_all(pty_at, (const uint8_t *)ready, sizeof ready - 1);

    for (;;) {
        uint8_t buf[512];
        ssize_t k = read(pty_at, buf, sizeof buf);
        if (k > 0) at_modem_input(&modem, buf, (size_t)k);
        if (pty_tnfs >= 0) {
            k = read(pty_tnfs, buf, sizeof buf);
            if (k > 0) tnfs_rx_feed(&tnfs_rx, buf, (size_t)k, tnfs_frame, NULL);
        }
        at_modem_poll(&modem);
        pump(2);
    }
}
