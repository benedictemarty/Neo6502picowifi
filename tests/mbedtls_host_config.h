/* mbedtls_host_config.h — mbedTLS du SDK compilé sur PC pour tester
   roots_ca_cb (US-T13) : même sous-ensemble X.509 que src/mbedtls_config.h
   (RSA, P-256, P-384, SHA-1/2), sans TLS ni entropie matérielle ; dates
   vérifiées par mbedTLS (gmtime_r disponible sur PC), allocateur remplaçable
   pour mesurer la mémoire. */
#ifndef MBEDTLS_HOST_CONFIG_H
#define MBEDTLS_HOST_CONFIG_H

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_FS_IO
#define MBEDTLS_ERROR_C

#define MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

#endif
