/* test_roots_ca_cb.c — roots_ca_cb contre mbedTLS (celui du SDK, compilé
   sur PC, tests/mbedtls_host_config.h) : vérification de chaînes locales
   (tests/fixtures, gen.sh) et réelles, refus, sujets jumeaux, échec
   d'allocation, et mesure du tas : pic pendant une vérification à la demande
   contre toutes les racines chargées d'avance (ancienne méthode généralisée).
   Tailles mesurées sur PC 64 bits : ordre de grandeur, pas les octets du RP2040. */
#include "../src/roots_ca_cb.h"
#include "mbedtls/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct roots_store roots_fixture;   /* fixtures/store.pem */

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("ÉCHEC l.%d : ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------ allocateur compteur */
static size_t cur, peak;
static int fail_allocs;          /* > 0 : les prochaines allocations échouent */

static void *count_calloc(size_t n, size_t sz)
{
    if (fail_allocs) return NULL;
    if (sz && n > (size_t)-1 / sz - 16) return NULL;
    size_t *p = calloc(1, n * sz + 16);
    if (!p) return NULL;
    p[0] = n * sz;
    cur += n * sz;
    if (cur > peak) peak = cur;
    return (char *)p + 16;
}

static void count_free(void *q)
{
    if (!q) return;
    size_t *p = (size_t *)((char *)q - 16);
    cur -= p[0];
    free(p);
}

/* ---------------------------------------------------- vérification */
struct seen { const struct roots_store *s; int root; };

static int vrfy(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)depth; (void)flags;
    struct seen *v = ctx;
    int r = roots_index_of(v->s, crt->raw.p);
    if (r >= 0) v->root = r;
    return 0;
}

/* Vérifie la chaîne du fichier ; renvoie les drapeaux mbedTLS (ou 0xffffffff
   si mbedTLS échoue sans drapeau), *root = racine du magasin retenue. */
static uint32_t verify(const struct roots_store *s, const char *file, const char *host, int *root, size_t *pk)
{
    size_t base = cur;
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    int ret = mbedtls_x509_crt_parse_file(&chain, file);
    CHECK(ret == 0, "lecture de %s : -0x%x", file, (unsigned)-ret);
    uint32_t flags = 0;
    struct seen v = { s, -1 };
    size_t before = cur;
    peak = cur;
    ret = mbedtls_x509_crt_verify_with_ca_cb(&chain, roots_ca_cb, (void *)s, &mbedtls_x509_crt_profile_default,
                                             host, &flags, vrfy, &v);
    if (pk) *pk = peak - before;
    mbedtls_x509_crt_free(&chain);   /* libère aussi le cache RSA (RN) posé sur les clés de la chaîne */
    CHECK(cur == base, "%s : %zu octets non libérés", file, cur - base);
    if (root) *root = v.root;
    if (ret != 0 && flags == 0) return 0xffffffffu;
    return flags;
}

static const char *name(const struct roots_store *s, int i) { return i >= 0 ? s->e[i].name : "(aucune)"; }

static void fixtures(void)
{
    const struct roots_store *s = &roots_fixture;
    int r;
    uint32_t f = verify(s, "fixtures/chain_a.pem", "test.example", &r, NULL);
    CHECK(f == 0 && r >= 0 && !strcmp(name(s, r), "Test Root A"), "chaîne 3 niveaux : flags 0x%x, racine %s", f, name(s, r));

    f = verify(s, "fixtures/chain_a.pem", "other.example", &r, NULL);
    CHECK(f & MBEDTLS_X509_BADCERT_CN_MISMATCH, "mauvais nom d'hôte accepté (flags 0x%x)", f);
    CHECK(!(f & MBEDTLS_X509_BADCERT_NOT_TRUSTED), "mauvais nom : chaîne pourtant approuvée attendue (0x%x)", f);

    f = verify(s, "fixtures/chain_t.pem", "twin.example", &r, NULL);
    CHECK(f == 0 && r >= 0 && !strcmp(name(s, r), "Test Twin Root"), "racines jumelles : flags 0x%x, racine %s", f, name(s, r));

    f = verify(s, "fixtures/chain_x.pem", "x.example", &r, NULL);
    CHECK(f & MBEDTLS_X509_BADCERT_NOT_TRUSTED, "racine inconnue acceptée (flags 0x%x)", f);
    CHECK(r == -1, "racine inconnue : racine du magasin %s signalée", name(s, r));

    f = verify(&roots_store, "fixtures/chain_a.pem", "test.example", &r, NULL);
    CHECK(f & MBEDTLS_X509_BADCERT_NOT_TRUSTED, "racine de test acceptée par le magasin réel (flags 0x%x)", f);

    /* feuille jumelle : le rappel propose bien les deux racines de même sujet */
    mbedtls_x509_crt leaf, *cand = NULL;
    mbedtls_x509_crt_init(&leaf);
    CHECK(mbedtls_x509_crt_parse_file(&leaf, "fixtures/chain_t.pem") == 0, "lecture chain_t");
    CHECK(roots_ca_cb((void *)s, &leaf, &cand) == 0 && cand && cand->next && !cand->next->next,
          "jumelles : 2 candidates attendues");
    mbedtls_x509_crt_free(cand); mbedtls_free(cand);

    fail_allocs = 1;                               /* échec d'allocation : fatal, rien de rendu */
    cand = (mbedtls_x509_crt *)&cand;
    CHECK(roots_ca_cb((void *)s, &leaf, &cand) == MBEDTLS_ERR_X509_ALLOC_FAILED && cand == NULL, "échec d'allocation");
    fail_allocs = 0;
    mbedtls_x509_crt_free(&leaf);

    mbedtls_x509_crt_init(&leaf);                  /* émetteur absent : aucune candidate, pas d'erreur */
    CHECK(mbedtls_x509_crt_parse_file(&leaf, "fixtures/chain_x.pem") == 0, "lecture chain_x");
    cand = (mbedtls_x509_crt *)&cand;
    CHECK(roots_ca_cb((void *)s, &leaf, &cand) == 0 && cand == NULL, "émetteur absent");
    mbedtls_x509_crt_free(&leaf);
}

/* Chaînes réelles capturées (gen.sh --real) : leurs feuilles expirent, on
   ignore donc EXPIRED/FUTURE ; tout autre drapeau est un échec. */
static size_t real_chains(void)
{
    static const struct { const char *file, *host, *root; } t[] = {
        { "fixtures/real_www.digicert.com.pem", "www.digicert.com", "DigiCert Global Root G2" },
        { "fixtures/real_github.com.pem", "github.com", "Sectigo Public Server Authentication Root E46" },
        { "fixtures/real_mimuma.pl.pem", "mimuma.pl", "ISRG Root X1" },
    };
    size_t worst = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int r;
        size_t pk;
        uint32_t f = verify(&roots_store, t[i].file, t[i].host, &r, &pk);
        f &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
        CHECK(f == 0, "%s : flags 0x%x", t[i].host, f);
        CHECK(r >= 0 && !strcmp(name(&roots_store, r), t[i].root), "%s : racine %s, attendue %s",
              t[i].host, name(&roots_store, r), t[i].root);
        printf("  %-18s → %-46s pic du tas pendant la vérification : %zu o\n", t[i].host, name(&roots_store, r), pk);
        if (pk > worst) worst = pk;
    }
    return worst;
}

/* Ancienne méthode généralisée : toutes les racines décodées d'avance. */
static void memory(size_t on_demand_peak)
{
    const struct roots_store *s = &roots_store;
    mbedtls_x509_crt all;
    size_t before = cur;
    mbedtls_x509_crt_init(&all);
    for (int i = 0; i < s->count; i++)
        CHECK(mbedtls_x509_crt_parse_der(&all, s->der + s->e[i].der_off, s->e[i].der_len) == 0, "décodage de %s", s->e[i].name);
    size_t copy = cur - before;
    mbedtls_x509_crt_free(&all);
    mbedtls_x509_crt_init(&all);
    for (int i = 0; i < s->count; i++)
        CHECK(mbedtls_x509_crt_parse_der_nocopy(&all, s->der + s->e[i].der_off, s->e[i].der_len) == 0, "décodage sans copie de %s", s->e[i].name);
    size_t nocopy = cur - before;
    mbedtls_x509_crt_free(&all);
    CHECK(cur == before, "fuite après décodage de toutes les racines");
    printf("  %d racines décodées d'avance : %zu o (copie) / %zu o (sans copie) ; à la demande : pic %zu o\n",
           s->count, copy, nocopy, on_demand_peak);
    CHECK(on_demand_peak * 10 < copy, "gain mémoire insuffisant : %zu contre %zu", on_demand_peak, copy);
}

int main(void)
{
    mbedtls_platform_set_calloc_free(count_calloc, count_free);
    fixtures();
    size_t pk = real_chains();
    memory(pk);
    CHECK(cur == 0, "%zu octets encore alloués à la fin", cur);
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
