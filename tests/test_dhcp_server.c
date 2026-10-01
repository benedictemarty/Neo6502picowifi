/*
 * test_dhcp_server.c — serveur DHCP du point d'accès de configuration (US-W6)
 * et DNS captif : tests sur PC.
 */
#include "../src/dhcp_server.h"
#include "../src/dns_catchall.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Message client : type DHCP, MAC (dernier octet), options 50/54 facultatives. */
static size_t msg(uint8_t *b, uint8_t type, uint8_t mac_last, uint32_t req_ip, uint32_t server_id)
{
    memset(b, 0, 400);
    b[0] = 1; b[1] = 1; b[2] = 6;
    b[4] = 0xde; b[5] = 0xad; b[6] = 0xbe; b[7] = 0xef;      /* xid */
    b[10] = 0x80;                                          /* broadcast */
    uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, mac_last };
    memcpy(b + 28, mac, 6);
    b[236] = 99; b[237] = 130; b[238] = 83; b[239] = 99;
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (req_ip) { *o++ = 50; *o++ = 4; o[0] = (uint8_t)(req_ip >> 24); o[1] = (uint8_t)(req_ip >> 16); o[2] = (uint8_t)(req_ip >> 8); o[3] = (uint8_t)req_ip; o += 4; }
    if (server_id) { *o++ = 54; *o++ = 4; o[0] = (uint8_t)(server_id >> 24); o[1] = (uint8_t)(server_id >> 16); o[2] = (uint8_t)(server_id >> 8); o[3] = (uint8_t)server_id; o += 4; }
    *o++ = 255;
    return (size_t)(o - b);
}

static int opt(const uint8_t *r, size_t len, uint8_t code, const uint8_t **v)
{
    size_t i = 240;
    while (i + 1 < len && r[i] != 255) {
        if (r[i] == code) { *v = r + i + 2; return r[i + 1]; }
        i += 2 + (size_t)r[i + 1];
    }
    return -1;
}

static void test_dhcp(void)
{
    struct dhcps s;
    uint8_t in[400], out[DHCPS_REPLY_MAX];
    const uint8_t *v;
    uint32_t srv = IP(192, 168, 4, 1);
    dhcps_init(&s, srv, IP(255, 255, 255, 0));

    /* DISCOVER → OFFER .16, options complètes */
    size_t n = dhcps_handle(&s, in, msg(in, 1, 1, 0, 0), out, sizeof out, 100);
    CHECK(n == DHCPS_REPLY_MAX);
    CHECK(out[0] == 2 && !memcmp(out + 4, in + 4, 4) && !memcmp(out + 28, in + 28, 6));
    CHECK(get32(out + 16) == IP(192, 168, 4, 16));
    CHECK(opt(out, n, 53, &v) == 1 && v[0] == 2);
    CHECK(opt(out, n, 54, &v) == 4 && get32(v) == srv);
    CHECK(opt(out, n, 1, &v) == 4 && get32(v) == IP(255, 255, 255, 0));
    CHECK(opt(out, n, 3, &v) == 4 && get32(v) == srv);
    CHECK(opt(out, n, 6, &v) == 4 && get32(v) == srv);
    CHECK(opt(out, n, 51, &v) == 4 && get32(v) == DHCPS_LEASE_S);

    /* REQUEST de l'adresse offerte → ACK ; même client → même adresse */
    n = dhcps_handle(&s, in, msg(in, 3, 1, IP(192, 168, 4, 16), srv), out, sizeof out, 101);
    CHECK(n && opt(out, n, 53, &v) == 1 && v[0] == 5 && get32(out + 16) == IP(192, 168, 4, 16));
    n = dhcps_handle(&s, in, msg(in, 1, 1, 0, 0), out, sizeof out, 102);
    CHECK(get32(out + 16) == IP(192, 168, 4, 16));

    /* second client → .17 ; REQUEST d'une autre adresse → NAK */
    n = dhcps_handle(&s, in, msg(in, 1, 2, 0, 0), out, sizeof out, 103);
    CHECK(get32(out + 16) == IP(192, 168, 4, 17));
    n = dhcps_handle(&s, in, msg(in, 3, 2, IP(192, 168, 4, 99), srv), out, sizeof out, 104);
    CHECK(n && opt(out, n, 53, &v) == 1 && v[0] == 6 && get32(out + 16) == 0);
    CHECK(opt(out, n, 51, &v) < 0);

    /* REQUEST destiné à un autre serveur : ignoré ; client inconnu → NAK */
    CHECK(dhcps_handle(&s, in, msg(in, 3, 2, IP(192, 168, 4, 17), IP(10, 0, 0, 1)), out, sizeof out, 105) == 0);
    n = dhcps_handle(&s, in, msg(in, 3, 9, IP(192, 168, 4, 20), srv), out, sizeof out, 105);
    CHECK(n && opt(out, n, 53, &v) == 1 && v[0] == 6);

    /* REQUEST sans option 50 : ciaddr (renouvellement) */
    msg(in, 3, 1, 0, 0);
    in[12] = 192; in[13] = 168; in[14] = 4; in[15] = 16;
    n = dhcps_handle(&s, in, 260, out, sizeof out, 106);
    CHECK(n && opt(out, n, 53, &v) == 1 && v[0] == 5);

    /* pool plein : 8 baux, le 9e client n'a pas de réponse ; libéré → réutilisé */
    for (uint8_t c = 3; c <= 8; c++) {
        n = dhcps_handle(&s, in, msg(in, 1, c, 0, 0), out, sizeof out, 110);
        uint32_t a = get32(out + 16);
        dhcps_handle(&s, in, msg(in, 3, c, a, srv), out, sizeof out, 110);
    }
    CHECK(dhcps_handle(&s, in, msg(in, 1, 50, 0, 0), out, sizeof out, 111) == 0);
    dhcps_handle(&s, in, msg(in, 7, 2, 0, 0), out, sizeof out, 112);            /* RELEASE .17 */
    n = dhcps_handle(&s, in, msg(in, 1, 50, 0, 0), out, sizeof out, 113);
    CHECK(n && get32(out + 16) == IP(192, 168, 4, 17));
    /* bail expiré : réutilisable par un autre client */
    n = dhcps_handle(&s, in, msg(in, 1, 51, 0, 0), out, sizeof out, 112 + DHCPS_LEASE_S + 10);
    CHECK(n && get32(out + 16) >= IP(192, 168, 4, 16) && get32(out + 16) <= IP(192, 168, 4, 23));

    /* messages invalides : ignorés */
    msg(in, 1, 1, 0, 0);
    CHECK(dhcps_handle(&s, in, 100, out, sizeof out, 200) == 0);              /* trop court */
    in[236] = 0;
    CHECK(dhcps_handle(&s, in, 260, out, sizeof out, 200) == 0);              /* cookie     */
    msg(in, 1, 1, 0, 0); in[0] = 2;
    CHECK(dhcps_handle(&s, in, 260, out, sizeof out, 200) == 0);              /* BOOTREPLY  */
    msg(in, 1, 1, 0, 0); in[241] = 200;                                       /* option hors limites */
    CHECK(dhcps_handle(&s, in, 260, out, sizeof out, 200) == 0);
    CHECK(dhcps_handle(&s, in, msg(in, 1, 1, 0, 0), out, 10, 200) == 0);     /* sortie trop petite */
}

static void test_dns(void)
{
    /* requête A pour connectivitycheck.gstatic.com */
    uint8_t q[64] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
    size_t n = 12;
    const char *labels[] = { "connectivitycheck", "gstatic", "com" };
    for (int i = 0; i < 3; i++) { q[n++] = (uint8_t)strlen(labels[i]); memcpy(q + n, labels[i], strlen(labels[i])); n += strlen(labels[i]); }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    uint8_t r[128];
    size_t k = dns_catchall(q, n, r, sizeof r, IP(192, 168, 4, 1));
    CHECK(k == n + 16);
    CHECK(r[0] == 0x12 && r[1] == 0x34 && (r[2] & 0x80) && (r[2] & 0x01) && (r[3] & 0x0f) == 0);
    CHECK(r[7] == 1 && !memcmp(r + 12, q + 12, n - 12));
    CHECK(r[n] == 0xc0 && r[n + 1] == 0x0c && r[n + 3] == 1 && r[n + 11] == 4);
    CHECK(get32(r + n + 12) == IP(192, 168, 4, 1));
    /* AAAA : réponse vide (pas d'IPv6) */
    q[n - 3] = 28;
    k = dns_catchall(q, n, r, sizeof r, IP(192, 168, 4, 1));
    CHECK(k == n && r[7] == 0);
    /* invalides : réponse, tronquée, nom compressé, sortie trop petite */
    q[n - 3] = 1;
    q[2] = 0x81; CHECK(dns_catchall(q, n, r, sizeof r, 1) == 0); q[2] = 0x01;
    CHECK(dns_catchall(q, n - 3, r, sizeof r, 1) == 0);
    CHECK(dns_catchall(q, 8, r, sizeof r, 1) == 0);
    CHECK(dns_catchall(q, n, r, n, 1) == 0);
    q[12] = 0xc0; CHECK(dns_catchall(q, n, r, sizeof r, 1) == 0);
    q[12] = 60; CHECK(dns_catchall(q, n, r, sizeof r, 1) == 0);          /* étiquette hors limites */
}

int main(void)
{
    test_dhcp();
    test_dns();
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
