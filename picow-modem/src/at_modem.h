/*
 * at_modem.h — cœur portable du modem Wi-Fi (Neo6502drive, US-T1/US-T2).
 *
 * Deux dialectes sur un même canal série :
 *  - sous-ensemble AT ESP8266 (Espressif AT 1.x) utilisé par netsetup /
 *    netinfo / netconsole / prophet (relevé dans leurs sources) ;
 *  - modem Hayes minimal : ATDT hôte:port, +++ (garde 1 s), ATO, ATH, ATA,
 *    ATE, ATZ, ATI, registres S0/S2/S12.
 *
 * Aucune dépendance matérielle : toute action réseau passe par at_modem_ops,
 * ce qui permet des tests unitaires sur PC (tests/test_at_modem.c).
 */
#ifndef AT_MODEM_H
#define AT_MODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AT_LINE_MAX      256   /* longueur max d'une ligne de commande       */
#define AT_SSID_MAX      32
#define AT_PASS_MAX      64
#define AT_HOST_MAX      64
#define AT_SEND_MAX      2048  /* AT+CIPSEND=n : n maximal (ESP8266 : 2048) */
#define AT_RX_RING_SIZE  8192  /* données TCP entrantes en attente          */

/* Codes de retour des opérations réseau. */
enum at_net_result {
    AT_NET_OK = 0,
    AT_NET_TIMEOUT,
    AT_NET_BAD_PASSWORD,
    AT_NET_NO_AP,
    AT_NET_DNS_FAIL,
    AT_NET_CONNECT_FAIL,
    AT_NET_FAIL,
};

/* Informations IP (chaînes déjà formatées, "0.0.0.0" si absent). */
struct at_ip_info {
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char mac[18];
    char ssid[AT_SSID_MAX + 1];
    int  channel;
    int  rssi;
    bool dhcp;
};

/* Configuration persistante (ESP : *_DEF). Sauvée par ops->config_save. */
struct at_config {
    uint32_t magic;
    char     ssid[AT_SSID_MAX + 1];
    char     pass[AT_PASS_MAX + 1];
    uint8_t  echo;          /* ATE0/ATE1                                     */
    uint8_t  dhcp;          /* 1 = DHCP, 0 = IP statique                     */
    char     static_ip[16];
    char     static_gw[16];
    char     static_mask[16];
    char     dns[16];       /* "" = DNS du DHCP                              */
    uint8_t  sntp_enable;
    int8_t   sntp_tz;
    char     sntp_server[64];
    uint16_t listen_port;   /* 0 = pas d'écoute (AT+CIPSERVER / ATS0)        */
    uint8_t  s0;            /* réponse automatique (sonneries)               */
};

#define AT_CONFIG_MAGIC 0x4E574D31u /* 'NWM1' */

/* Rappel d'énumération Wi-Fi : ecn (0 open, 2 WPA, 3 WPA2, 4 WPA/WPA2). */
typedef void (*at_scan_cb)(void *ctx, int ecn, const char *ssid, int rssi);

/* Opérations fournies par la plateforme (Pico W ou maquette de test). */
struct at_modem_ops {
    void *ctx;
    /* sortie série (vers tous les transports actifs) */
    void (*write)(void *ctx, const uint8_t *data, size_t len);
    uint32_t (*millis)(void *ctx);

    /* Wi-Fi */
    int  (*wifi_join)(void *ctx, const char *ssid, const char *pass);
    void (*wifi_leave)(void *ctx);
    bool (*wifi_connected)(void *ctx);
    int  (*wifi_scan)(void *ctx, at_scan_cb cb, void *cb_ctx);
    void (*ip_info)(void *ctx, struct at_ip_info *info);

    /* TCP sortant (une connexion, comme AT+CIPMUX=0) */
    int  (*tcp_connect)(void *ctx, const char *host, uint16_t port);
    int  (*tcp_send)(void *ctx, const uint8_t *data, size_t len);
    void (*tcp_close)(void *ctx);
    bool (*tcp_connected)(void *ctx);
    /* écoute entrante : port 0 = arrêter ; accept = accepter l'appel en attente */
    int  (*tcp_listen)(void *ctx, uint16_t port);
    bool (*tcp_accept)(void *ctx);

    /* divers */
    void (*config_save)(void *ctx, const struct at_config *cfg);
    void (*sntp_time)(void *ctx, char *out, size_t out_len); /* "" si inconnu */
    int  (*ping)(void *ctx, const char *host);               /* ms ou <0     */
    void (*reset)(void *ctx);
    const char *(*version)(void *ctx);
};

enum at_mode {
    AT_MODE_COMMAND = 0,   /* interprète les lignes AT                       */
    AT_MODE_CIPSEND,       /* collecte n octets après AT+CIPSEND=n           */
    AT_MODE_ONLINE,        /* Hayes : données transparentes (après ATDT/ATA) */
};

struct at_modem {
    const struct at_modem_ops *ops;
    struct at_config cfg;
    enum at_mode mode;

    char   line[AT_LINE_MAX];
    size_t line_len;
    bool   skip_lf;            /* '\r' vu : absorber le '\n' suivant        */

    /* AT+CIPSEND */
    uint8_t send_buf[AT_SEND_MAX];
    size_t  send_len, send_expected;

    /* Hayes */
    uint8_t  s2;               /* caractère d'échappement, 43 = '+'          */
    uint8_t  s12;              /* temps de garde en 1/50 s (50 = 1 s)        */
    int      plus_count;
    uint32_t last_rx_ms;       /* dernier octet reçu en ligne                */
    uint32_t plus_ms;          /* fin du 3e '+'                              */
    bool     escape_pending;   /* "+++" vu, attente du temps de garde        */
    bool     ring_pending;     /* appel entrant non répondu                  */
    int      ring_count;

    /* données TCP reçues, en attente d'émission (+IPD ou transparent) */
    uint8_t  rx_ring[AT_RX_RING_SIZE];
    volatile size_t rx_head, rx_tail;
    volatile bool remote_closed;
    bool     was_connected;
};

/* Initialisation ; cfg peut être NULL (valeurs par défaut). */
void at_modem_init(struct at_modem *m, const struct at_modem_ops *ops,
                   const struct at_config *cfg);
void at_modem_config_defaults(struct at_config *cfg);

/* Octets venant du 6502/PC. */
void at_modem_input(struct at_modem *m, const uint8_t *data, size_t len);

/* À appeler régulièrement (échappement +++, +IPD, CLOSED, RING). */
void at_modem_poll(struct at_modem *m);

/* Événements venant du réseau (peuvent être appelés depuis une IRQ). */
size_t at_modem_rx_space(const struct at_modem *m);
size_t at_modem_rx_push(struct at_modem *m, const uint8_t *data, size_t len);
void   at_modem_remote_closed(struct at_modem *m);
void   at_modem_ring(struct at_modem *m);

#endif
