/* roots_store.h — magasin de racines de confiance en flash (US-T13).
   Généré par tools/roots2c.py : DER concaténés + index trié par empreinte
   FNV-1a du sujet. Recherche portable, testée sur PC (tests/test_roots_store.c). */
#ifndef ROOTS_STORE_H
#define ROOTS_STORE_H
#include <stddef.h>
#include <stdint.h>

struct roots_entry {
    uint32_t hash;        /* FNV-1a 32 bits du sujet (DER brut)          */
    uint32_t der_off;     /* début du certificat dans roots_store.der     */
    uint16_t der_len;
    uint16_t subj_off;    /* sujet : relatif au début du certificat       */
    uint16_t subj_len;
    const char *name;     /* CN (sinon OU, sinon O), ASCII, pour ATI      */
};

struct roots_store {
    const uint8_t *der;
    uint32_t der_size;
    const struct roots_entry *e;   /* trié par hash croissant */
    int count;
};

/* Racines embarquées dans le firmware (certs/roots.pem). */
extern const struct roots_store roots_store;

uint32_t roots_hash(const uint8_t *p, size_t n);

/* Indices (dans s->e) des racines dont le sujet égale name (DER brut),
   au plus max ; renvoie le nombre écrit dans idx. */
int roots_find(const struct roots_store *s, const uint8_t *name, size_t n, int *idx, int max);

/* Indice de la racine dont le DER commence à p, -1 si p n'est pas dans s. */
int roots_index_of(const struct roots_store *s, const uint8_t *p);

#endif
