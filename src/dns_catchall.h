/*
 * dns_catchall.h — DNS « portail captif » du point d'accès de configuration
 * (US-W6) : toute question de type A reçoit l'adresse du point d'accès, ce
 * qui fait ouvrir la page de configuration par les téléphones. Portable,
 * testé sur PC (tests/test_web_setup.c).
 */
#ifndef DNS_CATCHALL_H
#define DNS_CATCHALL_H

#include <stddef.h>
#include <stdint.h>

/* Réponse à la requête in (port 53) ; ip en ordre hôte. Renvoie la longueur
   écrite dans out, 0 = pas de réponse (message invalide ou trop grand). */
size_t dns_catchall(const uint8_t *in, size_t len, uint8_t *out, size_t cap, uint32_t ip);

#endif
