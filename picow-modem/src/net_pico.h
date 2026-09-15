/* net_pico.h — opérations réseau du modem sur Pico W (cyw43 + lwIP). */
#ifndef NET_PICO_H
#define NET_PICO_H

#include "at_modem.h"

/* Initialise le Wi-Fi (cyw43) ; renvoie false si la puce ne répond pas. */
bool net_pico_init(struct at_modem *modem);

/* Opérations à passer à at_modem_init (write/millis sont remplis par main). */
extern struct at_modem_ops net_pico_ops;

/* À appeler dans la boucle principale (sonneries répétées, SNTP). */
void net_pico_poll(void);

/* Configuration persistante en flash (dernier secteur). */
void config_flash_load(struct at_config *cfg);
void config_flash_save(void *ctx, const struct at_config *cfg);

#endif
