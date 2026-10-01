/* tnfs_pico.h — second port USB (CDC 1) réservé à TNFS, sur Pico W. */
#ifndef TNFS_PICO_H
#define TNFS_PICO_H

#include <stdbool.h>

void tnfs_pico_init(void);
/* usb_descriptors.c : second port USB présent (à appeler avant tusb_init) */
void usb_descriptors_tnfs(bool enable);
void tnfs_pico_poll(void);   /* boucle principale, après tud_task() */

#endif
