/* mbedtls_pc_config.h — configuration mbedTLS du modem simulé sur PC : celle
   du firmware (src/mbedtls_config.h, mêmes suites et même magasin de racines),
   sauf ce qui dépend du Pico : entropie du système (getrandom/urandom),
   horloge en ms de la libc, et dates des certificats vérifiées par mbedTLS
   (sur le Pico : tls_verify_cb + heure SNTP). */
#ifndef MBEDTLS_PC_CONFIG_H
#define MBEDTLS_PC_CONFIG_H
#include "../src/mbedtls_config.h"
#undef MBEDTLS_NO_PLATFORM_ENTROPY
#undef MBEDTLS_ENTROPY_HARDWARE_ALT
#undef MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_HAVE_TIME_DATE
#endif
