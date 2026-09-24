/* roots_ca_cb.h — callback mbedTLS « CA de confiance à la demande » (US-T13).
   À installer avec mbedtls_ssl_conf_ca_cb(conf, roots_ca_cb, (void *)&roots_store). */
#ifndef ROOTS_CA_CB_H
#define ROOTS_CA_CB_H
#include "mbedtls/build_info.h"   /* avant tout en-tête : fixe MBEDTLS_PRIVATE selon la config */
#include "mbedtls/x509_crt.h"
#include "roots_store.h"

/* Racines de même sujet renvoyées au plus par appel (renouvellement de clé). */
#define ROOTS_MAX_CANDIDATES 4

/* ctx = const struct roots_store *. Candidats = racines dont le sujet égale
   l'émetteur de child, décodées sans copie (le DER reste en flash) ; aucune
   → *candidates = NULL, mbedTLS conclut « non approuvé ». mbedTLS libère
   la liste. Renvoie 0, ou MBEDTLS_ERR_X509_ALLOC_FAILED (échec fatal). */
int roots_ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates);

#endif
