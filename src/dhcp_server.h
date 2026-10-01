/*
 * dhcp_server.h — serveur DHCP minimal du point d'accès de configuration (US-W6).
 *
 * Le pilote cyw43 du SDK n'en fournit pas (CYW43_NETUTILS = 0). Portable :
 * une requête BOOTP/DHCP en entrée, la réponse à diffuser (port 68) en
 * sortie ; testé sur PC (tests/test_dhcp_server.c). Adresses en ordre hôte.
 */
#ifndef DHCP_SERVER_H
#define DHCP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#define DHCPS_LEASES   8       /* baux : serveur .16 à .23 du sous-réseau */
#define DHCPS_FIRST    16
#define DHCPS_LEASE_S  3600    /* durée d'un bail accordé                  */
#define DHCPS_OFFER_S  60      /* adresse réservée après une offre         */
#define DHCPS_REPLY_MAX 300    /* taille d'une réponse (BOOTP minimal)     */

struct dhcps {
    uint32_t ip, mask;                 /* adresse du point d'accès            */
    uint8_t  mac[DHCPS_LEASES][6];
    uint32_t expiry[DHCPS_LEASES];     /* en secondes ; <= maintenant = libre */
};

void dhcps_init(struct dhcps *s, uint32_t ip, uint32_t mask);

/* Traite un message reçu sur le port 67 ; renvoie la longueur de la réponse
   écrite dans out (0 = pas de réponse). cap >= DHCPS_REPLY_MAX. */
size_t dhcps_handle(struct dhcps *s, const uint8_t *in, size_t len,
                    uint8_t *out, size_t cap, uint32_t now_s);

#endif
