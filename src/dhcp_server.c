/*
 * dhcp_server.c — serveur DHCP minimal (voir dhcp_server.h).
 * DISCOVER → OFFER, REQUEST → ACK ou NAK, RELEASE/DECLINE → bail libéré.
 */
#include "dhcp_server.h"

#include <stdbool.h>
#include <string.h>

enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE };

#define OPT_START 240   /* options après l'en-tête BOOTP et le cookie */

static const uint8_t cookie[4] = { 99, 130, 83, 99 };

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static const uint8_t *find_opt(const uint8_t *in, size_t len, uint8_t code, uint8_t *olen)
{
    size_t i = OPT_START;
    while (i < len) {
        uint8_t c = in[i];
        if (c == 255) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t l = in[i + 1];
        if (i + 2 + l > len) break;
        if (c == code) { *olen = l; return in + i + 2; }
        i += 2 + (size_t)l;
    }
    return NULL;
}

static uint32_t lease_ip(const struct dhcps *s, int i)
{
    return (s->ip & s->mask) | (uint32_t)(DHCPS_FIRST + i);
}

/* Bail de cette adresse MAC (même expiré), sinon un bail libre si alloc. */
static int lease_for(struct dhcps *s, const uint8_t *mac, uint32_t now, bool alloc)
{
    for (int i = 0; i < DHCPS_LEASES; i++)
        if (!memcmp(s->mac[i], mac, 6)) return i;
    if (!alloc) return -1;
    for (int i = 0; i < DHCPS_LEASES; i++)
        if (s->expiry[i] <= now) { memcpy(s->mac[i], mac, 6); return i; }
    return -1;
}

void dhcps_init(struct dhcps *s, uint32_t ip, uint32_t mask)
{
    memset(s, 0, sizeof *s);
    s->ip = ip;
    s->mask = mask;
}

static size_t reply(const struct dhcps *s, const uint8_t *in, uint8_t type, uint32_t yiaddr,
                    uint8_t *out)
{
    memset(out, 0, DHCPS_REPLY_MAX);
    out[0] = 2;                       /* BOOTREPLY */
    out[1] = 1;                       /* Ethernet  */
    out[2] = 6;
    memcpy(out + 4, in + 4, 4);       /* xid       */
    memcpy(out + 10, in + 10, 2);     /* flags     */
    put32(out + 16, yiaddr);
    put32(out + 20, s->ip);           /* siaddr    */
    memcpy(out + 24, in + 24, 4);     /* giaddr    */
    memcpy(out + 28, in + 28, 16);    /* chaddr    */
    memcpy(out + 236, cookie, 4);
    uint8_t *o = out + OPT_START;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 54; *o++ = 4; put32(o, s->ip); o += 4;
    if (type != NAK) {
        *o++ = 51; *o++ = 4; put32(o, DHCPS_LEASE_S); o += 4;
        *o++ = 1;  *o++ = 4; put32(o, s->mask); o += 4;
        *o++ = 3;  *o++ = 4; put32(o, s->ip); o += 4;    /* routeur        */
        *o++ = 6;  *o++ = 4; put32(o, s->ip); o += 4;    /* DNS : captif   */
    }
    *o++ = 255;
    return DHCPS_REPLY_MAX;
}

size_t dhcps_handle(struct dhcps *s, const uint8_t *in, size_t len,
                    uint8_t *out, size_t cap, uint32_t now_s)
{
    if (cap < DHCPS_REPLY_MAX || len < OPT_START || in[0] != 1 || in[1] != 1 || in[2] != 6
        || memcmp(in + 236, cookie, 4) != 0) return 0;
    uint8_t l;
    const uint8_t *t = find_opt(in, len, 53, &l);
    if (!t || l != 1) return 0;
    const uint8_t *mac = in + 28;
    int i;
    switch (t[0]) {
    case DISCOVER:
        i = lease_for(s, mac, now_s, true);
        if (i < 0) return 0;                              /* plus d'adresse libre */
        if (s->expiry[i] < now_s + DHCPS_OFFER_S) s->expiry[i] = now_s + DHCPS_OFFER_S;
        return reply(s, in, OFFER, lease_ip(s, i), out);
    case REQUEST: {
        const uint8_t *sid = find_opt(in, len, 54, &l);
        if (sid && (l != 4 || get32(sid) != s->ip)) return 0;   /* autre serveur choisi */
        const uint8_t *req = find_opt(in, len, 50, &l);
        uint32_t want = (req && l == 4) ? get32(req) : get32(in + 12);
        i = lease_for(s, mac, now_s, false);
        if (i < 0 || want != lease_ip(s, i)) return reply(s, in, NAK, 0, out);
        s->expiry[i] = now_s + DHCPS_LEASE_S;
        return reply(s, in, ACK, lease_ip(s, i), out);
    }
    case DECLINE:
    case RELEASE:
        i = lease_for(s, mac, now_s, false);
        if (i >= 0) { memset(s->mac[i], 0, 6); s->expiry[i] = 0; }
        return 0;
    default:
        return 0;
    }
}
