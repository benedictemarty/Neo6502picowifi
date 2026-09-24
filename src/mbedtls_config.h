/*
 * mbedtls_config.h — mbedTLS 3.6 pour le modem Pico W : client TLS 1.2 seul.
 *
 * Jeu de suites validé sur RP2040 (ECDHE-ECDSA / ECDHE-RSA / RSA, AES-GCM et
 * CBC, SHA-256/384), vérification X.509 contre les racines embarquées, SNI,
 * tickets de session (une connexion TLS par bloc « Range » de prophet.neo).
 *
 * Pas de MBEDTLS_HAVE_TIME_DATE : la vérification des dates par mbedTLS
 * appelle gmtime_r (newlib), qui prend un verrou et fige la puce lorsqu'elle
 * s'exécute dans le contexte lwIP (interruption). Les dates sont vérifiées
 * dans un rappel de net_pico.c avec une conversion arithmétique.
 */
#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#include <limits.h>

/* plateforme */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_TIME                 /* tickets : mbedtls_time() → time() (net_pico.c) */
#define MBEDTLS_PLATFORM_MS_TIME_ALT      /* mbedtls_ms_time() : net_pico.c (pas de clock_gettime) */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT      /* pico_mbedtls.c : mbedtls_hardware_poll (pico_rand) */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_ERROR_C                   /* messages d'erreur dans le journal ATI */
#define MBEDTLS_DEBUG_C                   /* journal mbedTLS filtré (alertes) vers ATI */
#define MBEDTLS_SELF_TEST                 /* AT+TLSTEST : autotests des primitives sur carte */

/* TLS client 1.2 */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_IN_CONTENT_LEN     8192
#define MBEDTLS_SSL_OUT_CONTENT_LEN    2048
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* courbes et clés publiques — fenêtre de précalcul plus large : ECDSA/ECDHE
   nettement plus rapides sur Cortex-M0+ contre quelques Ko de RAM */
#define MBEDTLS_ECP_WINDOW_SIZE 4
#define MBEDTLS_ECP_FIXED_POINT_OPTIM 1
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* X.509 */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK  /* US-T13 : racines en flash, roots_ca_cb */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* chiffrement et hachage */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C                    /* chaînes anciennes / empreintes */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

#endif
