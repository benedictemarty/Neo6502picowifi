/* dns_catchall.c — voir dns_catchall.h. */
#include "dns_catchall.h"

#include <string.h>

size_t dns_catchall(const uint8_t *in, size_t len, uint8_t *out, size_t cap, uint32_t ip)
{
    if (len < 12 || (in[2] & 0x80) || (in[2] & 0x78)) return 0;  /* réponse ou opcode ≠ requête */
    if (in[4] != 0 || in[5] != 1) return 0;                      /* une seule question         */
    size_t q = 12;
    while (q < len && in[q]) {
        if (in[q] & 0xc0) return 0;                              /* pas de compression ici     */
        q += 1 + (size_t)in[q];
    }
    q++;                                                         /* octet nul final            */
    if (q + 4 > len) return 0;
    int is_a = in[q] == 0 && in[q + 1] == 1 && in[q + 2] == 0 && in[q + 3] == 1;
    q += 4;
    size_t n = q + (is_a ? 16 : 0);
    if (n > cap) return 0;
    memcpy(out, in, q);
    out[2] = (uint8_t)(0x84 | (in[2] & 0x01));                   /* QR, AA, RD recopié         */
    out[3] = 0x80;                                               /* RA, pas d'erreur           */
    out[6] = 0; out[7] = (uint8_t)is_a;                          /* ANCOUNT                    */
    memset(out + 8, 0, 4);                                       /* NSCOUNT, ARCOUNT           */
    if (is_a) {
        static const uint8_t ans[12] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
        memcpy(out + q, ans, sizeof ans);
        out[q + 12] = (uint8_t)(ip >> 24); out[q + 13] = (uint8_t)(ip >> 16);
        out[q + 14] = (uint8_t)(ip >> 8);  out[q + 15] = (uint8_t)ip;
    }
    return n;
}
