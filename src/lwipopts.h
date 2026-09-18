/* lwipopts.h — lwIP sans OS (pico_cyw43_arch_lwip_threadsafe_background). */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    16000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETCONN                0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define ETH_PAD_SIZE                0
#define LWIP_CHKSUM_ALGORITHM       3
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_STATS                  0

/* TLS terminé sur le Pico W : altcp + mbedTLS (mbedtls_config.h) ; mbedTLS
   alloue sur le tas newlib, pas dans MEM_SIZE. */
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
/* altcp_tls met VERIFY_OPTIONAL par défaut : un certificat invalide (nom,
   chaîne, dates) laisserait passer la connexion. Exigé : REQUIRED. */
#define ALTCP_MBEDTLS_AUTHMODE      MBEDTLS_SSL_VERIFY_REQUIRED
#define ALTCP_MBEDTLS_USE_SESSION_TICKETS 0   /* côté serveur seulement ; le client réutilise la session via altcp_tls_get/set_session */

/* Le client SNTP (et lwIP lui-même pour TCP/DNS/DHCP) réserve des sys_timeout
   au-delà du compte interne ; sans cette marge, le premier tcp_connect après
   sntp_init() échoue sur « pool MEMP_SYS_TIMEOUT is empty » (vu sur carte). */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 6)

/* SNTP : l'heure reçue est confiée à net_pico.c */
#define SNTP_SERVER_DNS             1
#define SNTP_UPDATE_DELAY           3600000
void net_pico_set_time(unsigned int sec);
#define SNTP_SET_SYSTEM_TIME(sec)   net_pico_set_time(sec)

/* Diagnostic : les messages LWIP_DEBUGF d'altcp_tls (échecs de handshake,
   code mbedTLS) sont conservés pour ATI (net_pico.c : net_pico_diag). */
#define LWIP_DEBUG                  1
#define ALTCP_MBEDTLS_DEBUG         LWIP_DBG_ON
#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_ON
void net_pico_diag(const char *fmt, ...);
#define LWIP_PLATFORM_DIAG(x)       net_pico_diag x
#define LWIP_PLATFORM_ASSERT_INCLUDE_ONCE 1

/* Une assertion lwIP devient panic() dans le SDK (boucle infinie, puis reset
   par le watchdog) ; on conserve le message pour ATI (net_pico.c). */
void net_pico_lwip_assert(const char *msg);
#define LWIP_PLATFORM_ASSERT(x)     net_pico_lwip_assert(x)

#endif
