/* ap_pico.h — point d'accès de configuration sur Pico W (US-W6). */
#ifndef AP_PICO_H
#define AP_PICO_H

#include <stdbool.h>

/* Ouverture automatique : au démarrage sans réseau mémorisé, ou si le réseau
   mémorisé reste injoignable AP_AUTO_OPEN_MS après le démarrage. */
#define AP_AUTO_OPEN_MS   60000
#define AP_IDLE_CLOSE_MS  600000   /* fermeture après 10 min sans requête HTTP  */
#define AP_OK_CLOSE_MS    15000    /* fermeture après une association réussie   */

int  ap_pico_setup(void *ctx, int on);      /* at_modem_ops.ap_setup      */
const char *ap_pico_ssid(void *ctx);        /* at_modem_ops.ap_setup_ssid */
void ap_pico_poll(void);                    /* boucle principale          */

#endif
