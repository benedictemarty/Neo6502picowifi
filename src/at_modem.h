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
#define AT_RX_RING_SIZE  8192  /* données TCP/UDP entrantes en attente      */
#define AT_UDP_MAX       1472  /* datagramme UDP maximal (CIPSEND et +IPD)  */

/* Codes de retour des opérations réseau. */
enum at_net_result {
    AT_NET_OK = 0,
    AT_NET_TIMEOUT,
    AT_NET_BAD_PASSWORD,
    AT_NET_NO_AP,
    AT_NET_DNS_FAIL,
    AT_NET_CONNECT_FAIL,
    AT_NET_FAIL,
    AT_NET_NO_TIME,        /* TLS refusé : heure SNTP non acquise           */
    AT_NET_TLS_FAIL,       /* handshake / certificat refusé                 */
    AT_NET_NO_TLS,         /* TLS non compilé sur cette plateforme          */
};

#define AT_TLS_PORTS_MAX 4
#define AT_HOSTS_MAX     8     /* US-T12 : hôtes autorisés (page web seulement) */
#define AT_LOG_MAX       16    /* US-T12 : journal des connexions              */

/* Informations IP (chaînes déjà formatées, "0.0.0.0" si absent). */
struct at_ip_info {
    char ip[16];
    char gateway[16];
    char netmask[16];
    char dns[16];
    char mac[18];
    char bssid[18];   /* MAC du point d'accès (AT+CWJAP?) */
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
    uint16_t tls_ports[AT_TLS_PORTS_MAX]; /* AT+TLSPORT : CIPSTART "TCP" → TLS */
    char     ap_pass[AT_PASS_MAX + 1];    /* AT+APSETUPPWD : point d'accès de configuration */
    char     tnfs_host[AT_HOST_MAX + 1];  /* AT$TNFS : serveur du port USB TNFS ("" = aucun) */
    uint16_t tnfs_port;
    uint8_t  tnfs_usb;      /* AT$TNFSUSB : 1 = second port USB TNFS (au démarrage) */
    /* US-T12 : filtrage des hôtes. Modifiable seulement depuis la page web du
       point d'accès (un programme 6502 ne peut pas élargir sa propre liste). */
    uint8_t  hosts_enforce;
    char     hosts[AT_HOSTS_MAX][AT_HOST_MAX + 1];  /* "nom", "*.domaine" ou IP */
};

#define AT_CONFIG_MAGIC    0x4E574D33u /* 'NWM3' */
#define AT_CONFIG_MAGIC_V2 0x4E574D32u /* 'NWM2' : même préfixe, sans ap_pass   */
#define AT_CONFIG_MAGIC_V1 0x4E574D31u /* 'NWM1' : même préfixe, sans tls_ports */
#define AT_AP_PASS_DEFAULT "neo6502wifi"  /* documenté dans le README (US-W6) */

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

    /* Lien sortant unique (AT+CIPMUX=0). tcp_connect ouvre un lien TCP
       (tls = TLS terminé ici), udp_connect un lien UDP ; tcp_send /
       tcp_close / tcp_connected s'appliquent au lien ouvert, quel qu'il soit
       (en UDP, tcp_send émet un datagramme). */
    int  (*tcp_connect)(void *ctx, const char *host, uint16_t port, bool tls);
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
    void (*bootsel)(void *ctx);   /* AT+BOOTSEL : mode UF2 (NULL = non supporté) */
    const char *(*version)(void *ctx);
    const char *(*boot_info)(void *ctx);  /* ATI : cause du dernier reset (NULL = rien) */
    const char *(*tls_info)(void *ctx);   /* ATI : pile TLS, racine, heure (NULL = pas de TLS) */
    const char *(*tls_selftest)(void *ctx); /* AT+TLSTEST : autotests des primitives (NULL = absent) */
    const char *(*build)(void *ctx);      /* ATI : identifiant de build, git describe (NULL = rien) */
    const char *(*build_date)(void *ctx); /* AT+GMR « compile time » : date du commit (NULL = unknown) */
    int  (*udp_connect)(void *ctx, const char *host, uint16_t port); /* NULL = UDP non supporté */
    /* Point d'accès de configuration (US-W6) : on = 1 ouvre, 0 ferme ; renvoie
       AT_NET_OK. ap_setup_ssid : SSID si ouvert, NULL si fermé. NULL = absent. */
    int  (*ap_setup)(void *ctx, int on);
    const char *(*ap_setup_ssid)(void *ctx);
};

/* Vrai si le port est dans la liste AT+TLSPORT. */
bool at_modem_port_is_tls(const struct at_config *cfg, uint16_t port);

/* US-T12 : vrai si le filtrage est inactif ou si host est dans la liste
   (nom exact sans casse, ou "*.domaine" pour ses sous-domaines). */
bool at_modem_host_allowed(const struct at_config *cfg, const char *host);
/* Entrée de liste valide : 1 à 64 caractères [A-Za-z0-9.-], "*." en tête admis. */
bool at_modem_host_pattern_valid(const char *pattern);

struct at_log_entry {
    uint32_t ms;               /* instant (millis) de la tentative */
    char     kind[6];          /* "TCP", "SSL", "UDP", "DIAL", "PING", "TNFS", "SNTP", "IN" */
    char     host[AT_HOST_MAX + 1];
    uint16_t port;
    bool     allowed;
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
    uint8_t  plus_held[3];     /* '+' retenus tant que l'échappement est possible */
    bool     ring_pending;     /* appel entrant non répondu                  */
    int      ring_count;

    /* données TCP reçues, en attente d'émission (+IPD ou transparent) */
    uint8_t  rx_ring[AT_RX_RING_SIZE];
    volatile size_t rx_head, rx_tail;
    volatile bool remote_closed;
    bool     was_connected;
    bool     link_udp;         /* lien ouvert en UDP : tampon en datagrammes */

    /* US-T12 : dernières tentatives de connexion (anneau) */
    struct at_log_entry log[AT_LOG_MAX];
    unsigned log_count;
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
/* Datagramme UDP entier (1..AT_UDP_MAX octets) : rendu en un seul +IPD ;
   tout ou rien, renvoie 0 si le tampon est plein (datagramme perdu). */
size_t at_modem_rx_push_dgram(struct at_modem *m, const uint8_t *data, size_t len);
void   at_modem_remote_closed(struct at_modem *m);
void   at_modem_ring(struct at_modem *m);

/* US-T12 : consigne une tentative (aussi utilisé par la plateforme : TNFS). */
void   at_modem_log(struct at_modem *m, const char *kind, const char *host, uint16_t port, bool allowed);
/* i-ème entrée, de la plus récente (0) à la plus ancienne ; NULL au-delà. */
const struct at_log_entry *at_modem_log_get(const struct at_modem *m, unsigned i);

#endif
