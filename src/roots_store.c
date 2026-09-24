/* roots_store.c — recherche dans le magasin de racines en flash (US-T13). */
#include "roots_store.h"
#include <string.h>

uint32_t roots_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    while (n--) h = (h ^ *p++) * 0x01000193u;
    return h;
}

int roots_find(const struct roots_store *s, const uint8_t *name, size_t n, int *idx, int max)
{
    if (!s || !name || max <= 0) return 0;
    uint32_t h = roots_hash(name, n);
    int lo = 0, hi = s->count;             /* première entrée de hash >= h */
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (s->e[mid].hash < h) lo = mid + 1; else hi = mid;
    }
    int k = 0;
    for (int i = lo; i < s->count && s->e[i].hash == h && k < max; i++) {
        const struct roots_entry *r = &s->e[i];
        if (r->subj_len == n && !memcmp(s->der + r->der_off + r->subj_off, name, n))
            idx[k++] = i;                  /* même hash, sujet différent : collision ignorée */
    }
    return k;
}

int roots_index_of(const struct roots_store *s, const uint8_t *p)
{
    if (!s || p < s->der || p >= s->der + s->der_size) return -1;
    uint32_t off = (uint32_t)(p - s->der);
    for (int i = 0; i < s->count; i++)
        if (s->e[i].der_off == off) return i;
    return -1;
}
