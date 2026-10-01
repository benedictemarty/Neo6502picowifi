/*
 * test_at_modem.c — tests unitaires du cœur du modem sur PC (gcc).
 * Maquette des opérations réseau ; vérifie les séquences attendues par
 * netsetup/netinfo/prophet et le comportement Hayes (ATDT, +++, ATH, ATO).
 */
#include "../src/at_modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------ maquette */

static struct {
    char out[16384];
    size_t out_len;
    uint32_t ms;
    bool wifi_up, tcp_up, listen_pending;
    int  join_result, connect_result;
    char last_ssid[64], last_pass[64], last_host[64];
    bool last_tls, no_tls, no_time;
    uint16_t last_port, listen_port;
    uint8_t sent[4096];
    size_t sent_len;
    int saved;
    int resets;
    int sends;                   /* appels à tcp_send (un datagramme en UDP) */
    bool last_udp;
    bool ap_on;
    struct at_config saved_cfg;
} M;

static void mw(void *c, const uint8_t *d, size_t n) { (void)c; memcpy(M.out + M.out_len, d, n); M.out_len += n; M.out[M.out_len] = 0; }
static uint32_t mms(void *c) { (void)c; return M.ms; }
static int mjoin(void *c, const char *s, const char *p) { (void)c; strcpy(M.last_ssid, s); strcpy(M.last_pass, p); M.wifi_up = (M.join_result == AT_NET_OK); return M.join_result; }
static void mleave(void *c) { (void)c; M.wifi_up = false; }
static bool mwifi(void *c) { (void)c; return M.wifi_up; }
static int mscan(void *c, at_scan_cb cb, void *x) { (void)c; cb(x, 3, "Livebox-1234", -55); cb(x, 0, "Free WiFi", -80); return AT_NET_OK; }
static void minfo(void *c, struct at_ip_info *i) {
    (void)c; memset(i, 0, sizeof *i);
    strcpy(i->ip, "192.168.1.42"); strcpy(i->gateway, "192.168.1.1"); strcpy(i->netmask, "255.255.255.0");
    strcpy(i->dns, "192.168.1.1"); strcpy(i->mac, "28:cd:c1:00:11:22"); strcpy(i->bssid, "aa:bb:cc:dd:ee:ff"); strcpy(i->ssid, "Livebox-1234");
    i->channel = 6; i->rssi = -55; i->dhcp = true;
}
static int mconn(void *c, const char *h, uint16_t p, bool tls) {
    (void)c; strcpy(M.last_host, h); M.last_port = p; M.last_tls = tls;
    if (tls && M.no_tls) return AT_NET_NO_TLS;
    if (tls && M.no_time) return AT_NET_NO_TIME;
    M.tcp_up = (M.connect_result == AT_NET_OK); return M.connect_result; }
static const char *mtls(void *c) { (void)c; return M.no_tls ? NULL : "TLS: test 1.2, root: Test Root, time: synced"; }
static int msend(void *c, const uint8_t *d, size_t n) { (void)c; memcpy(M.sent + M.sent_len, d, n); M.sent_len += n; M.sends++; return AT_NET_OK; }
static int map(void *c, int on) { (void)c; M.ap_on = on; return AT_NET_OK; }
static const char *mapssid(void *c) { (void)c; return M.ap_on ? "Neo6502-modem-1122" : NULL; }
static int mudp(void *c, const char *h, uint16_t p) {
    (void)c; strcpy(M.last_host, h); M.last_port = p; M.last_udp = true;
    M.tcp_up = (M.connect_result == AT_NET_OK); return M.connect_result; }
static void mclose(void *c) { (void)c; M.tcp_up = false; }
static bool mtcp(void *c) { (void)c; return M.tcp_up; }
static int mlisten(void *c, uint16_t p) { (void)c; M.listen_port = p; return AT_NET_OK; }
static bool maccept(void *c) { (void)c; if (!M.listen_pending) return false; M.listen_pending = false; M.tcp_up = true; return true; }
static void msave(void *c, const struct at_config *cfg) { (void)c; M.saved++; M.saved_cfg = *cfg; }
static void msntp(void *c, char *o, size_t n) { (void)c; snprintf(o, n, "Tue Sep 15 12:00:00 2026"); }
static int mping(void *c, const char *h) { (void)c; return strcmp(h, "nowhere") ? 12 : -1; }
static void mreset(void *c) { (void)c; M.resets++; }
static int bootsels;
static void mbootsel(void *c) { (void)c; bootsels++; }
static const char *mver(void *c) { (void)c; return "0.1.0"; }

static const struct at_modem_ops ops = {
    NULL, mw, mms, mjoin, mleave, mwifi, mscan, minfo, mconn, msend, mclose, mtcp,
    mlisten, maccept, msave, msntp, mping, mreset, mbootsel, mver, NULL, mtls, NULL, NULL, NULL, mudp, map, mapssid,
};

static struct at_modem modem;
static int failures, checks;

static void reset_mock(void)
{
    memset(&M, 0, sizeof M);
    M.join_result = AT_NET_OK;
    M.connect_result = AT_NET_OK;
    at_modem_init(&modem, &ops, NULL);
}

static void send(const char *s) { at_modem_input(&modem, (const uint8_t *)s, strlen(s)); at_modem_poll(&modem); }
static void clear_out(void) { M.out_len = 0; M.out[0] = 0; }

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_OUT(needle) do { checks++; if (!strstr(M.out, needle)) { failures++; printf("ÉCHEC %s:%d : sortie sans « %s » :\n%s\n", __FILE__, __LINE__, needle, M.out); } } while (0)
#define CHECK_NOT_OUT(needle) do { checks++; if (strstr(M.out, needle)) { failures++; printf("ÉCHEC %s:%d : sortie contient « %s » :\n%s\n", __FILE__, __LINE__, needle, M.out); } } while (0)

/* ------------------------------------------------------------ tests */

static void test_basic(void)
{
    reset_mock();
    send("AT\r\n");
    CHECK_OUT("AT\r\n");            /* écho actif par défaut */
    CHECK_OUT("\r\nOK\r\n");
    clear_out();
    send("ATE0\r\n");
    CHECK_OUT("ATE0\r\n");           /* l'ESP échoue encore la commande ATE0 elle-même */
    CHECK_OUT("OK");
    clear_out();
    send("AT\r\n");
    CHECK(strcmp(M.out, "\r\nOK\r\n") == 0);
    clear_out();
    send("\r\n\r\n");                 /* lignes vides ignorées (prophet envoie CRLF après les données) */
    CHECK(M.out_len == 0);
    send("XYZ\r\n");
    CHECK_OUT("ERROR");
    clear_out();
    send("AT+NIMPORTEQUOI\r\n");
    CHECK_OUT("ERROR");
    clear_out();
    send("at+gmr\r\n");               /* minuscules acceptées */
    CHECK_OUT("AT version:");
    CHECK_OUT("OK");
}

static void test_netinfo_sequence(void)
{
    reset_mock();
    send("ATE0\r\n"); clear_out();
    send("AT+CWMODE?\r\n");
    CHECK_OUT("+CWMODE:1\r\n");
    clear_out();
    send("AT+CIPSTATUS\r\n");
    CHECK_OUT("STATUS:5\r\n");         /* pas d'AP */
    clear_out();
    M.wifi_up = true;
    send("AT+CIPSTATUS\r\n");
    CHECK_OUT("STATUS:2\r\n");
    clear_out();
    send("AT+CWJAP_CUR?\r\n");
    CHECK_OUT("+CWJAP_CUR:\"Livebox-1234\",\"aa:bb:cc:dd:ee:ff\",6,-55\r\n");
    clear_out();
    send("AT+CIPSTA_CUR?\r\n");
    CHECK_OUT("+CIPSTA_CUR:ip:\"192.168.1.42\"\r\n+CIPSTA_CUR:gateway:\"192.168.1.1\"\r\n+CIPSTA_CUR:netmask:\"255.255.255.0\"\r\n");
    clear_out();
    send("AT+CIPDNS_CUR?\r\n");
    CHECK_OUT("+CIPDNS_CUR:192.168.1.1\r\n");
    clear_out();
    send("AT+CIFSR\r\n");
    CHECK_OUT("+CIFSR:STAIP,\"192.168.1.42\"\r\n+CIFSR:STAMAC,\"28:cd:c1:00:11:22\"\r\n");
    clear_out();
    send("AT+CWDHCP_DEF?\r\n");
    CHECK_OUT("+CWDHCP_DEF:3\r\n");    /* bit 1 = DHCP station actif */
    clear_out();
    send("AT+CIPSSLCCONF?\r\n");
    CHECK_OUT("+CIPSSLCCONF:2\r\n");   /* CA toujours vérifiée */
    clear_out();
    send("AT+CIPSNTPCFG?\r\n");
    CHECK_OUT("+CIPSNTPCFG:0,0,\"pool.ntp.org\"\r\n");
    clear_out();
    send("AT+CIPSNTPCFG=1,2,\"fr.pool.ntp.org\"\r\n");
    CHECK_OUT("OK");
    CHECK(M.saved_cfg.sntp_enable == 1 && M.saved_cfg.sntp_tz == 2 && !strcmp(M.saved_cfg.sntp_server, "fr.pool.ntp.org"));
    clear_out();
    send("AT+CIPSNTPTIME?\r\n");
    CHECK_OUT("+CIPSNTPTIME:Tue Sep 15 12:00:00 2026\r\n");
}

static void test_netsetup_join_scan(void)
{
    reset_mock();
    send("ATE0\r\n"); clear_out();
    send("AT+CWLAPOPT=1,7\r\n");
    CHECK_OUT("OK");
    clear_out();
    send("AT+CWLAP=,,,1,,\r\n");
    CHECK_OUT("+CWLAP:(3,\"Livebox-1234\",-55)\r\n+CWLAP:(0,\"Free WiFi\",-80)\r\n");
    CHECK_OUT("OK");
    clear_out();
    send("AT+CWJAP_DEF=\"Livebox-1234\",\"motdepasse\"\r\n");
    CHECK(!strcmp(M.last_ssid, "Livebox-1234") && !strcmp(M.last_pass, "motdepasse"));
    CHECK_OUT("WIFI CONNECTED\r\nWIFI GOT IP\r\n");
    CHECK_OUT("OK");
    CHECK(M.saved == 1 && !strcmp(M.saved_cfg.ssid, "Livebox-1234"));
    clear_out();
    M.join_result = AT_NET_BAD_PASSWORD;
    send("AT+CWJAP_DEF=\"Livebox-1234\",\"faux\"\r\n");
    CHECK_OUT("+CWJAP:2\r\n");
    CHECK_OUT("FAIL");
    CHECK_NOT_OUT("OK");
    clear_out();
    M.join_result = AT_NET_OK;
    send("AT+CWJAP_CUR=\"Autre\",\"x\"\r\n"); /* _CUR : non persistant */
    CHECK(M.saved == 2 && !strcmp(M.saved_cfg.ssid, "Livebox-1234"));
    clear_out();
    send("AT+CWQAP\r\n");
    CHECK_OUT("OK");
    CHECK_OUT("WIFI DISCONNECT");
    CHECK(!M.wifi_up);
    clear_out();
    send("AT+CIPSTA_DEF=\"192.168.1.200\",\"192.168.1.1\",\"255.255.255.0\"\r\n");
    CHECK_OUT("OK");
    CHECK(!M.saved_cfg.dhcp && !strcmp(M.saved_cfg.static_ip, "192.168.1.200"));
    clear_out();
    send("AT+CWDHCP_DEF=1,1\r\n");
    CHECK(M.saved_cfg.dhcp == 1);
    clear_out();
    send("AT+CIPDNS_DEF=1,\"1.1.1.1\"\r\n");
    CHECK(!strcmp(M.saved_cfg.dns, "1.1.1.1"));
    send("AT+CIPDNS_DEF=0\r\n");
    CHECK(M.saved_cfg.dns[0] == 0);
    clear_out();
    send("AT+CWMODE=1\r\n"); CHECK_OUT("OK"); clear_out();
    send("AT+CWMODE=2\r\n"); CHECK_OUT("ERROR"); clear_out(); /* pas de point d'accès */
    send("AT+CIUPDATE\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+RST\r\n"); CHECK_OUT("OK"); CHECK(M.resets == 1); clear_out();
    send("AT+RESTORE\r\n"); CHECK(M.resets == 2 && M.saved_cfg.ssid[0] == 0); clear_out();
    send("AT+BOOTSEL\r\n"); CHECK_OUT("OK"); CHECK(bootsels == 1);
}

static void test_prophet_http(void)
{
    reset_mock();
    M.wifi_up = true;
    send("ATE0\r\n"); clear_out();
    /* TCPConnect */
    send("AT+CIPSTART=\"TCP\",\"mimuma.pl\",8998\r\n");
    CHECK(!strcmp(M.last_host, "mimuma.pl") && M.last_port == 8998);
    CHECK_OUT("CONNECT\r\n");
    CHECK_OUT("OK");
    clear_out();
    /* CIPSTART alors que connecté : comme l'ESP, pas de OK (prophet ne le vérifie pas) */
    send("AT+CIPSTART=\"TCP\",\"mimuma.pl\",8998\r\n");
    CHECK_OUT("ALREADY CONNECTED");
    CHECK_OUT("ERROR");
    CHECK_NOT_OUT("OK");
    clear_out();
    send("AT+CIPSTATUS\r\n");
    CHECK_OUT("STATUS:3\r\n");
    clear_out();
    /* MakeGetRequest : AT+CIPSEND=n, EmptyBuffer, puis données + CRLF */
    const char *req = "GET /list HTTP/1.1 \r\nHost: mimuma.pl\r\nResponseFormat: cli\r\n\r\n";
    char cmd[64];
    snprintf(cmd, sizeof cmd, "AT+CIPSEND=%u\r\n", (unsigned)strlen(req));
    send(cmd);
    CHECK_OUT("\r\nOK\r\n> ");
    clear_out();
    send(req);
    CHECK(M.sent_len == strlen(req) && !memcmp(M.sent, req, M.sent_len));
    CHECK_OUT("Recv ");
    CHECK_OUT("SEND OK");
    clear_out();
    send("\r\n");                      /* CRLF ajouté par SendStringToUart : ignoré */
    CHECK(M.out_len == 0);
    /* réponse HTTP arrivant du réseau, en deux segments */
    const char *rsp1 = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\n";
    CHECK(at_modem_rx_push(&modem, (const uint8_t *)rsp1, strlen(rsp1)) == strlen(rsp1));
    at_modem_poll(&modem);
    char expect[128];
    snprintf(expect, sizeof expect, "\r\n+IPD,%u:%s", (unsigned)strlen(rsp1), rsp1);
    CHECK(!strcmp(M.out, expect));
    clear_out();
    CHECK(at_modem_rx_push(&modem, (const uint8_t *)"hello", 5) == 5);
    at_modem_remote_closed(&modem);
    at_modem_poll(&modem);
    CHECK(!strcmp(M.out, "\r\n+IPD,5:helloCLOSED\r\n"));
    M.tcp_up = false;
    clear_out();
    send("AT+CIPSTATUS\r\n");
    CHECK_OUT("STATUS:4\r\n");        /* TCP fermée */
    clear_out();
    send("AT+CIPCLOSE\r\n");
    CHECK_OUT("ERROR");               /* rien à fermer */
    clear_out();
    /* reconnexion puis fermeture explicite */
    send("AT+CIPSTART=\"TCP\",\"mimuma.pl\",8998\r\n"); clear_out();
    send("AT+CIPCLOSE\r\n");
    CHECK_OUT("CLOSED\r\n");
    CHECK_OUT("OK");
    CHECK(!M.tcp_up);
    clear_out();
    M.connect_result = AT_NET_DNS_FAIL;
    send("AT+CIPSTART=\"TCP\",\"inconnu.invalid\",80\r\n");
    CHECK_OUT("DNS Fail");
    CHECK_OUT("ERROR");
    clear_out();
    M.connect_result = AT_NET_OK;
    send("AT+CIPSEND=10\r\n");
    CHECK_OUT("link is not valid");
    CHECK_OUT("ERROR");
    clear_out();
    send("AT+CIPSEND=0\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+CIPSEND=9999\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+CIPSTART=\"UDP\",\"x\",1\r\n"); CHECK_OUT("CONNECT"); CHECK(M.last_udp); clear_out();
    send("AT+CIPCLOSE\r\n"); CHECK_OUT("CLOSED"); clear_out();
    M.wifi_up = false;
    send("AT+CIPSTART=\"UDP\",\"x\",1\r\n"); CHECK_OUT("no ip"); CHECK_OUT("ERROR"); clear_out();
    send("AT+CIPSTART=\"FOO\",\"x\",1\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+CIPMUX=0\r\n"); CHECK_OUT("OK"); clear_out();
    send("AT+CIPMUX=1\r\n"); CHECK_OUT("ERROR"); clear_out();
}

static void test_rx_ring(void)
{
    reset_mock();
    uint8_t big[AT_RX_RING_SIZE];
    memset(big, 'x', sizeof big);
    CHECK(at_modem_rx_space(&modem) == AT_RX_RING_SIZE - 1);
    CHECK(at_modem_rx_push(&modem, big, AT_RX_RING_SIZE) == 0);          /* trop grand */
    CHECK(at_modem_rx_push(&modem, big, AT_RX_RING_SIZE - 1) == AT_RX_RING_SIZE - 1);
    CHECK(at_modem_rx_space(&modem) == 0);
    CHECK(at_modem_rx_push(&modem, big, 1) == 0);
    send("ATE0\r\n"); /* poll vide le tampon en blocs +IPD */
    CHECK(at_modem_rx_space(&modem) == AT_RX_RING_SIZE - 1);
    CHECK_OUT("+IPD,1460:");
}

static void test_hayes(void)
{
    reset_mock();
    send("ATE0\r\n"); clear_out();
    send("ATI\r\n");
    CHECK_OUT("Neo6502drive Pico W modem 0.1.0");
    clear_out();
    send("ATDT bbs.example.org:2323\r\n");
    CHECK_OUT("NO CARRIER");           /* pas de Wi-Fi */
    clear_out();
    M.wifi_up = true;
    send("ATDT bbs.example.org:2323\r\n");
    CHECK(!strcmp(M.last_host, "bbs.example.org") && M.last_port == 2323);
    CHECK_OUT("CONNECT\r\n");
    CHECK(modem.mode == AT_MODE_ONLINE);
    clear_out();
    /* données transparentes dans les deux sens */
    send("hello");
    CHECK(M.sent_len == 5 && !memcmp(M.sent, "hello", 5));
    at_modem_rx_push(&modem, (const uint8_t *)"world", 5);
    at_modem_poll(&modem);
    CHECK(!strcmp(M.out, "world"));
    clear_out();
    /* "+++" sans temps de garde avant : transmis, pas d'échappement */
    M.ms += 100;
    send("+++");
    M.ms += 2000;
    at_modem_poll(&modem);
    CHECK(modem.mode == AT_MODE_ONLINE);
    CHECK(M.sent_len == 8);
    /* garde avant mais suivi d'autres octets : les '+' retenus sont transmis, dans l'ordre */
    M.ms += 2000;
    send("++x");
    CHECK(M.sent_len == 11 && !memcmp(M.sent + 8, "++x", 3));
    /* garde avant, "++" puis silence : transmis après le temps de garde */
    M.ms += 2000;
    send("++");
    CHECK(M.sent_len == 11);              /* retenus */
    M.ms += 1100;
    at_modem_poll(&modem);
    CHECK(M.sent_len == 13 && modem.mode == AT_MODE_ONLINE);
    /* "+++" avec garde avant et après : retour en mode commande, rien transmis */
    M.ms += 2000;
    send("+++");
    CHECK(modem.mode == AT_MODE_ONLINE); /* pas encore : garde après */
    CHECK(M.sent_len == 13);
    M.ms += 1100;
    at_modem_poll(&modem);
    CHECK(modem.mode == AT_MODE_COMMAND);
    CHECK(M.sent_len == 13);
    CHECK_OUT("OK");
    clear_out();
    send("ATO\r\n");
    CHECK_OUT("CONNECT");
    CHECK(modem.mode == AT_MODE_ONLINE);
    /* fermeture distante en ligne */
    at_modem_remote_closed(&modem);
    M.tcp_up = false;
    at_modem_poll(&modem);
    CHECK_OUT("NO CARRIER");
    CHECK(modem.mode == AT_MODE_COMMAND);
    clear_out();
    send("ATO\r\n");
    CHECK_OUT("NO CARRIER");
    clear_out();
    /* ATDT hôte port (espace), port par défaut 23, ATH */
    send("ATDT bbs.example.org 4000\r\n");
    CHECK(M.last_port == 4000);
    M.ms += 2000; send("+++"); M.ms += 1100; at_modem_poll(&modem);
    clear_out();
    send("ATH\r\n");
    CHECK_OUT("OK");
    CHECK(!M.tcp_up);
    clear_out();
    send("ATDbbs.example.org\r\n");
    CHECK(M.last_port == 23);
    M.ms += 2000; send("+++"); M.ms += 1100; at_modem_poll(&modem); clear_out();
    send("ATH\r\n"); clear_out();
    /* registres S */
    send("ATS12?\r\n"); CHECK_OUT("050\r\n"); clear_out();
    send("ATS12=20\r\n"); CHECK_OUT("OK"); CHECK(modem.s12 == 20); clear_out();
    send("ATS99=1\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("ATZ\r\n"); CHECK(modem.s12 == 50); clear_out();
    /* appel entrant : RING puis ATA */
    send("AT+CIPSERVER=1,6502\r\n");
    CHECK_OUT("OK");
    CHECK(M.listen_port == 6502 && M.saved_cfg.listen_port == 6502);
    clear_out();
    at_modem_ring(&modem);
    M.listen_pending = true;
    at_modem_poll(&modem);
    CHECK_OUT("RING");
    CHECK(modem.mode == AT_MODE_COMMAND);
    clear_out();
    send("ATA\r\n");
    CHECK_OUT("CONNECT");
    CHECK(modem.mode == AT_MODE_ONLINE && M.tcp_up);
    M.ms += 2000; send("+++"); M.ms += 1100; at_modem_poll(&modem); clear_out();
    send("ATH\r\n"); clear_out();
    /* réponse automatique S0=1 */
    send("ATS0=1\r\n"); clear_out();
    at_modem_ring(&modem);
    M.listen_pending = true;
    at_modem_poll(&modem);
    CHECK_OUT("RING");
    CHECK_OUT("CONNECT");
    CHECK(modem.mode == AT_MODE_ONLINE);
    /* AT+PING */
    M.ms += 2000; send("+++"); M.ms += 1100; at_modem_poll(&modem); clear_out();
    send("AT+PING=\"mimuma.pl\"\r\n"); CHECK_OUT("+12\r\n"); CHECK_OUT("OK"); clear_out();
    send("AT+PING=\"nowhere\"\r\n"); CHECK_OUT("ERROR"); clear_out();
}

static void test_tls(void)
{
    reset_mock();
    M.wifi_up = true;
    send("ATE0\r\n"); clear_out();
    /* netsetup : AT+CIPSSLCCONF=2 accepté, =1/=3 (cert client) refusés, =0 toléré */
    send("AT+CIPSSLCCONF=2\r\n"); CHECK_OUT("OK"); clear_out();
    send("AT+CIPSSLCCONF=0\r\n"); CHECK_OUT("OK"); clear_out();
    send("AT+CIPSSLCCONF=1\r\n"); CHECK_OUT("ERROR"); clear_out();
    /* CIPSTART "TCP" ordinaire : pas de TLS */
    send("AT+CIPSTART=\"TCP\",\"mimuma.pl\",8998\r\n");
    CHECK(!M.last_tls); CHECK_OUT("CONNECT"); clear_out();
    send("AT+CIPCLOSE\r\n"); clear_out();
    /* CIPSTART "SSL" explicite */
    send("AT+CIPSTART=\"SSL\",\"prophet.example.org\",443\r\n");
    CHECK(M.last_tls && M.last_port == 443); CHECK_OUT("CONNECT"); clear_out();
    send("AT+CIPCLOSE\r\n"); clear_out();
    /* AT+TLSPORT : "TCP" vers un port listé → TLS (prophet.neo non modifié, set port 443) */
    send("AT+TLSPORT?\r\n"); CHECK_OUT("+TLSPORT:\r\n"); clear_out();
    send("AT+TLSPORT=443,8443\r\n"); CHECK_OUT("OK");
    CHECK(M.saved_cfg.tls_ports[0] == 443 && M.saved_cfg.tls_ports[1] == 8443 && M.saved_cfg.tls_ports[2] == 0);
    clear_out();
    send("AT+TLSPORT?\r\n"); CHECK_OUT("+TLSPORT:443,8443\r\n"); clear_out();
    send("AT+CIPSTART=\"TCP\",\"prophet.example.org\",443\r\n");
    CHECK(M.last_tls); CHECK_OUT("CONNECT"); clear_out();
    send("AT+CIPCLOSE\r\n"); clear_out();
    send("AT+CIPSTART=\"TCP\",\"prophet.example.org\",80\r\n");
    CHECK(!M.last_tls); clear_out();
    send("AT+CIPCLOSE\r\n"); clear_out();
    /* ATDT vers un port listé → TLS aussi */
    send("ATDT prophet.example.org:8443\r\n"); CHECK(M.last_tls); CHECK_OUT("CONNECT");
    M.ms += 2000; send("+++"); M.ms += 1100; at_modem_poll(&modem); clear_out();
    send("ATH\r\n"); clear_out();
    /* ATI liste les ports TLS */
    send("ATI\r\n"); CHECK_OUT("TLS: test 1.2"); CHECK_OUT("TLS ports: 443 8443\r\n"); clear_out();
    /* trop de ports / valeur invalide */
    send("AT+TLSPORT=1,2,3,4,5\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+TLSPORT=70000\r\n"); CHECK_OUT("ERROR"); clear_out();
    /* effacement */
    send("AT+TLSPORT=0\r\n"); CHECK_OUT("OK"); CHECK(M.saved_cfg.tls_ports[0] == 0); clear_out();
    /* refus sans heure SNTP, échec de handshake */
    M.no_time = true;
    send("AT+CIPSTART=\"SSL\",\"prophet.example.org\",443\r\n");
    CHECK_OUT("no time (SNTP) for TLS"); CHECK_OUT("ERROR"); CHECK_NOT_OUT("CONNECT"); clear_out();
    M.no_time = false; M.connect_result = AT_NET_TLS_FAIL;
    send("AT+CIPSTART=\"SSL\",\"prophet.example.org\",443\r\n");
    CHECK_OUT("TLS handshake failed"); CHECK_OUT("ERROR"); clear_out();
    M.connect_result = AT_NET_OK;
    /* plateforme sans TLS : "SSL" → no TLS, CIPSSLCCONF? → 0 */
    M.no_tls = true;
    send("AT+CIPSTART=\"SSL\",\"x\",443\r\n"); CHECK_OUT("no TLS"); CHECK_OUT("ERROR"); clear_out();
    send("AT+CIPSSLCCONF=2\r\n"); CHECK_OUT("ERROR"); clear_out();
}

static void test_config_persist(void)
{
    struct at_config cfg;
    at_modem_config_defaults(&cfg);
    strcpy(cfg.ssid, "Reseau");
    cfg.echo = 0;
    reset_mock();
    at_modem_init(&modem, &ops, &cfg);
    CHECK(!strcmp(modem.cfg.ssid, "Reseau") && modem.cfg.echo == 0);
    send("AT\r\n");
    CHECK(!strcmp(M.out, "\r\nOK\r\n"));   /* pas d'écho */
    cfg.magic = AT_CONFIG_MAGIC_V1;           /* flash v1 : identifiants conservés, tls_ports à zéro */
    cfg.tls_ports[0] = 443;
    at_modem_init(&modem, &ops, &cfg);
    CHECK(!strcmp(modem.cfg.ssid, "Reseau") && modem.cfg.tls_ports[0] == 0 && modem.cfg.magic == AT_CONFIG_MAGIC);
    CHECK(!strcmp(modem.cfg.ap_pass, AT_AP_PASS_DEFAULT));
    cfg.magic = AT_CONFIG_MAGIC_V2;           /* flash v2 : tout conservé, ap_pass par défaut */
    cfg.tls_ports[0] = 443;
    memset(cfg.ap_pass, 0xff, sizeof cfg.ap_pass);   /* octets de flash effacée */
    at_modem_init(&modem, &ops, &cfg);
    CHECK(!strcmp(modem.cfg.ssid, "Reseau") && modem.cfg.tls_ports[0] == 443 && modem.cfg.magic == AT_CONFIG_MAGIC);
    CHECK(!strcmp(modem.cfg.ap_pass, AT_AP_PASS_DEFAULT));
    cfg.magic = AT_CONFIG_MAGIC;              /* v3 abîmée : mot de passe invalide → défaut */
    at_modem_init(&modem, &ops, &cfg);
    CHECK(!strcmp(modem.cfg.ap_pass, AT_AP_PASS_DEFAULT));
    cfg.magic = 0;                            /* flash vierge : défauts */
    at_modem_init(&modem, &ops, &cfg);
    CHECK(modem.cfg.ssid[0] == 0 && modem.cfg.echo == 1);
}

/* US-W3 : Bin version = fichier VERSION ; ATI ajoute l'identifiant de build s'il est fourni. */
static const char *mbuild(void *c) { (void)c; return "v0.1.0-3-gabc1234-dirty"; }
static const char *mdate(void *c) { (void)c; return "Sep 24 2026 14:10:58 UTC"; }

static void test_version(void)
{
    reset_mock();
    send("ATE0\r\n");
    clear_out();
    send("AT+GMR\r\n");
    CHECK_OUT("AT version:1.7.4.0(Neo6502drive)\r\n");
    CHECK_OUT("SDK version:0.1.0\r\n");
    CHECK_OUT("Bin version(Pico W):0.1.0\r\n");
    CHECK_OUT("compile time:unknown\r\n");               /* US-W5 : pas de __DATE__ ; ops.build_date = NULL */
    CHECK_OUT("OK");
    clear_out();
    send("ATI\r\n");
    CHECK_OUT("Neo6502drive Pico W modem 0.1.0\r\n");
    CHECK_NOT_OUT("build:");                              /* ops.build = NULL */

    struct at_modem_ops with_build = ops;
    with_build.build = mbuild;
    with_build.build_date = mdate;
    at_modem_init(&modem, &with_build, NULL);
    send("ATE0\r\n");
    clear_out();
    send("ATI\r\n");
    CHECK_OUT("Neo6502drive Pico W modem 0.1.0\r\nbuild: v0.1.0-3-gabc1234-dirty\r\n");
    CHECK_OUT("OK");
    clear_out();
    send("AT+GMR\r\n");
    CHECK_OUT("Bin version(Pico W):0.1.0\r\n");         /* GMR reste au format ESP, sans build */
    CHECK_OUT("compile time:Sep 24 2026 14:10:58 UTC\r\n");
    CHECK_NOT_OUT("dirty");
}

/* Datagrammes UDP : séquence du transport TNFS de reload-emulator. */
static void test_udp(void)
{
    reset_mock();
    M.wifi_up = true;
    send("AT\r\nATE0\r\n"); clear_out();
    send("AT+CIPCLOSE\r\n"); CHECK_OUT("ERROR"); clear_out();   /* pas de lien */

    /* plateforme sans UDP : ERROR */
    struct at_modem_ops no_udp = ops;
    no_udp.udp_connect = NULL;
    at_modem_init(&modem, &no_udp, NULL);
    send("AT+CIPSTART=\"UDP\",\"tnfs.example\",16384\r\n");
    CHECK_OUT("ERROR"); CHECK(!M.last_udp); clear_out();
    at_modem_init(&modem, &ops, NULL);
    send("ATE0\r\n"); clear_out();

    /* restes d'un lien TCP fermé : jetés à l'ouverture du lien UDP */
    at_modem_rx_push(&modem, (const uint8_t *)"vieux", 5);
    M.connect_result = AT_NET_DNS_FAIL;
    send("AT+CIPSTART=\"UDP\",\"inconnu.invalid\",16384\r\n");
    CHECK_OUT("DNS Fail"); CHECK_OUT("ERROR"); clear_out();
    M.connect_result = AT_NET_OK;
    send("AT+CIPSTART=\"UDP\",\"tnfs.example\",16384\r\n");
    CHECK(strcmp(M.out, "CONNECT\r\n\r\nOK\r\n") == 0);
    CHECK(M.last_udp && !strcmp(M.last_host, "tnfs.example") && M.last_port == 16384);
    CHECK_NOT_OUT("vieux");
    clear_out();
    send("AT+CIPSTATUS\r\n"); CHECK_OUT("STATUS:3"); CHECK_OUT("\"UDP\""); clear_out();
    send("AT+CIPSTART=\"UDP\",\"tnfs.example\",16384\r\n"); CHECK_OUT("ALREADY CONNECTED"); clear_out();

    /* envoi : un datagramme par CIPSEND, 1472 octets au plus */
    send("AT+CIPSEND=1473\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+CIPSEND=4\r\n");
    CHECK(strcmp(M.out, "\r\nOK\r\n> ") == 0); clear_out();
    /* réponse arrivée pendant la saisie : retenue jusqu'à SEND OK */
    CHECK(at_modem_rx_push_dgram(&modem, (const uint8_t *)"rep1", 4) == 4);
    at_modem_input(&modem, (const uint8_t *)"\x00\x01\x02", 3); at_modem_poll(&modem);
    CHECK(M.out_len == 0);
    at_modem_input(&modem, (const uint8_t *)"\x03", 1); at_modem_poll(&modem);
    CHECK(M.sends == 1 && M.sent_len == 4 && M.sent[3] == 3);
    CHECK(strcmp(M.out, "\r\nRecv 4 bytes\r\n\r\nSEND OK\r\n\r\n+IPD,4:rep1") == 0);
    clear_out();

    /* réception : un +IPD par datagramme, jamais regroupés ni coupés */
    uint8_t big[AT_UDP_MAX];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
    CHECK(at_modem_rx_push_dgram(&modem, (const uint8_t *)"ab", 2) == 2);
    CHECK(at_modem_rx_push_dgram(&modem, (const uint8_t *)"cde", 3) == 3);
    CHECK(at_modem_rx_push_dgram(&modem, big, 532) == 532);
    CHECK(at_modem_rx_push_dgram(&modem, big, AT_UDP_MAX) == AT_UDP_MAX);
    CHECK(at_modem_rx_push_dgram(&modem, big, AT_UDP_MAX + 1) == 0);   /* trop grand */
    CHECK(at_modem_rx_push_dgram(&modem, big, 0) == 0);
    at_modem_poll(&modem);
    const char *exp = "\r\n+IPD,2:ab\r\n+IPD,3:cde\r\n+IPD,532:";
    CHECK(memcmp(M.out, exp, strlen(exp)) == 0);
    size_t off = strlen(exp);
    CHECK(memcmp(M.out + off, big, 532) == 0);
    off += 532;
    CHECK(memcmp(M.out + off, "\r\n+IPD,1472:", 12) == 0);
    off += 12;
    CHECK(memcmp(M.out + off, big, AT_UDP_MAX) == 0);
    CHECK(M.out_len == off + AT_UDP_MAX);
    CHECK(at_modem_rx_space(&modem) == AT_RX_RING_SIZE - 1);
    clear_out();

    /* tampon plein : datagramme refusé en entier, les précédents restent intacts */
    size_t pushed = 0;
    while (at_modem_rx_push_dgram(&modem, big, AT_UDP_MAX)) pushed++;
    CHECK(pushed == (AT_RX_RING_SIZE - 1) / (AT_UDP_MAX + 2));
    at_modem_poll(&modem);
    size_t ipd = 0;
    for (size_t i = 0; i + 10 <= M.out_len; i++) ipd += !memcmp(M.out + i, "+IPD,1472:", 10);
    CHECK(ipd == pushed);
    clear_out();

    /* pas de mode transparent ni de décroché en UDP */
    send("ATO\r\n"); CHECK_OUT("NO CARRIER"); clear_out();
    M.listen_pending = true;
    send("ATA\r\n"); CHECK_OUT("NO CARRIER"); CHECK(M.listen_pending); clear_out();
    M.listen_pending = false;

    /* fermeture : datagrammes non lus jetés, retour au TCP en flux */
    at_modem_rx_push_dgram(&modem, (const uint8_t *)"perdu", 5);
    at_modem_input(&modem, (const uint8_t *)"AT+CIPCLOSE\r\n", 13);   /* sans poll avant */
    at_modem_poll(&modem);
    CHECK_NOT_OUT("perdu"); CHECK_OUT("CLOSED"); CHECK_OUT("OK"); CHECK(!M.tcp_up);
    clear_out();
    M.last_udp = false;
    send("AT+CIPSTART=\"TCP\",\"example.com\",80\r\n"); CHECK_OUT("CONNECT"); CHECK(!M.last_udp); clear_out();
    at_modem_rx_push(&modem, (const uint8_t *)"flux", 4);
    at_modem_poll(&modem);
    CHECK(strcmp(M.out, "\r\n+IPD,4:flux") == 0);
    send("AT+CIPSTATUS\r\n"); CHECK_OUT("\"TCP\""); clear_out();
}

/* US-W6 : point d'accès de configuration */
static void test_ap_setup(void)
{
    reset_mock();
    send("ATE0\r\n"); clear_out();
    send("AT+APSETUP?\r\n"); CHECK_OUT("+APSETUP:0\r\n"); CHECK_OUT("OK"); clear_out();
    send("ATI\r\n"); CHECK_OUT("setup AP: off\r\n"); clear_out();
    send("AT+APSETUP=1\r\n"); CHECK_OUT("OK"); CHECK(M.ap_on); clear_out();
    send("AT+APSETUP?\r\n"); CHECK_OUT("+APSETUP:1,\"Neo6502-modem-1122\"\r\n"); clear_out();
    send("ATI\r\n");
    CHECK_OUT("setup AP: \"Neo6502-modem-1122\", password \"" AT_AP_PASS_DEFAULT "\", http://192.168.4.1/\r\n");
    clear_out();
    send("AT+APSETUP=0\r\n"); CHECK_OUT("OK"); CHECK(!M.ap_on); clear_out();
    send("AT+APSETUP=2\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+APSETUP=x\r\n"); CHECK_OUT("ERROR"); clear_out();

    /* mot de passe : 8 à 63 caractères, persistant */
    send("AT+APSETUPPWD?\r\n"); CHECK_OUT("+APSETUPPWD:\"" AT_AP_PASS_DEFAULT "\""); clear_out();
    int saved = M.saved;
    send("AT+APSETUPPWD=\"court\"\r\n"); CHECK_OUT("ERROR"); CHECK(M.saved == saved); clear_out();
    send("AT+APSETUPPWD=\"0123456789012345678901234567890123456789012345678901234567890123\"\r\n");
    CHECK_OUT("ERROR"); clear_out();                                            /* 64 caractères */
    send("AT+APSETUPPWD=\"mon secret\"\r\n"); CHECK_OUT("OK");
    CHECK(M.saved == saved + 1 && !strcmp(M.saved_cfg.ap_pass, "mon secret")); clear_out();
    send("AT+APSETUPPWD?\r\n"); CHECK_OUT("+APSETUPPWD:\"mon secret\""); clear_out();

    /* plateforme sans point d'accès : ERROR, pas de ligne dans ATI */
    struct at_modem_ops no_ap = ops;
    no_ap.ap_setup = NULL;
    no_ap.ap_setup_ssid = NULL;
    at_modem_init(&modem, &no_ap, NULL);
    send("AT+APSETUP=1\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT+APSETUP?\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("ATI\r\n"); CHECK_NOT_OUT("setup AP"); clear_out();
}

/* AT$TNFS : serveur du second port USB (TNFS) ; AT&W */
static void test_tnfs_config(void)
{
    reset_mock();
    send("ATE0\r\n"); clear_out();
    CHECK(modem.cfg.tnfs_port == 16384 && modem.cfg.tnfs_host[0] == 0);
    send("AT$TNFS?\r\n"); CHECK_OUT("$TNFS:\"\",0\r\n"); CHECK_OUT("OK"); clear_out();
    send("ATI\r\n"); CHECK_OUT("TNFS (USB port 2): off\r\n"); clear_out();
    int saved = M.saved;
    send("AT$TNFS=\"tnfs.example\",16385\r\n"); CHECK_OUT("OK"); clear_out();
    CHECK(M.saved == saved + 1 && !strcmp(M.saved_cfg.tnfs_host, "tnfs.example") && M.saved_cfg.tnfs_port == 16385);
    send("AT$TNFS?\r\n"); CHECK_OUT("$TNFS:\"tnfs.example\",16385\r\n"); clear_out();
    send("ATI\r\n"); CHECK_OUT("TNFS (USB port 2): tnfs.example:16385\r\n"); clear_out();
    send("at$tnfs=192.168.1.10:16384\r\n"); CHECK_OUT("OK"); clear_out();   /* minuscules, hôte:port */
    CHECK(!strcmp(modem.cfg.tnfs_host, "192.168.1.10") && modem.cfg.tnfs_port == 16384);
    send("AT$TNFS=\"serveur\"\r\n"); CHECK_OUT("OK"); CHECK(modem.cfg.tnfs_port == 16384); clear_out();
    send("AT$TNFS=autre\r\n"); CHECK_OUT("OK"); CHECK(!strcmp(modem.cfg.tnfs_host, "autre")); clear_out();
    /* erreurs : port invalide, hôte vide, reste de ligne, commande inconnue */
    send("AT$TNFS=h:0\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT$TNFS=h:70000\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT$TNFS=\"\",1\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT$TNFS=h:12x\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT$TNFS=a b\r\n"); CHECK_OUT("ERROR"); clear_out();
    send("AT$FOO\r\n"); CHECK_OUT("ERROR"); clear_out();
    CHECK(!strcmp(modem.cfg.tnfs_host, "autre"));
    /* effacement ; AT&W accepté (rien à faire) */
    send("AT$TNFS=0\r\n"); CHECK_OUT("OK"); CHECK(modem.cfg.tnfs_host[0] == 0); clear_out();
    send("AT&W\r\n"); CHECK_OUT("OK"); clear_out();
    send("AT&X\r\n"); CHECK_OUT("ERROR"); clear_out();
    /* migration v2 : serveur TNFS vide, port par défaut */
    struct at_config cfg;
    at_modem_config_defaults(&cfg);
    strcpy(cfg.tnfs_host, "x");
    cfg.magic = AT_CONFIG_MAGIC_V2;
    at_modem_init(&modem, &ops, &cfg);
    CHECK(modem.cfg.tnfs_host[0] == 0 && modem.cfg.tnfs_port == 16384);
}

int main(void)
{
    test_basic();
    test_netinfo_sequence();
    test_netsetup_join_scan();
    test_prophet_http();
    test_rx_ring();
    test_udp();
    test_ap_setup();
    test_tnfs_config();
    test_hayes();
    test_tls();
    test_config_persist();
    test_version();
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
