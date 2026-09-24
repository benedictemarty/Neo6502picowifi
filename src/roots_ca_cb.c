/* roots_ca_cb.c — seules les racines utiles sont décodées en RAM (US-T13).
   Testé sur PC contre mbedTLS (tests/test_roots_ca_cb.c). */
#include "roots_ca_cb.h"
#include "mbedtls/platform.h"

int roots_ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates)
{
    const struct roots_store *s = ctx;
    int idx[ROOTS_MAX_CANDIDATES];
    *candidates = NULL;
    int n = roots_find(s, child->issuer_raw.p, child->issuer_raw.len, idx, ROOTS_MAX_CANDIDATES);
    if (n == 0) return 0;
    mbedtls_x509_crt *c = mbedtls_calloc(1, sizeof *c);
    if (!c) return MBEDTLS_ERR_X509_ALLOC_FAILED;
    mbedtls_x509_crt_init(c);
    int parsed = 0;
    for (int i = 0; i < n; i++) {
        const struct roots_entry *r = &s->e[idx[i]];
        if (mbedtls_x509_crt_parse_der_nocopy(c, s->der + r->der_off, r->der_len) == 0) parsed++;
    }
    if (!parsed) { mbedtls_x509_crt_free(c); mbedtls_free(c); return 0; }
    *candidates = c;
    return 0;
}
