/* tnfs_pico.h — second port USB (CDC 1) réservé à TNFS, sur Pico W. */
#ifndef TNFS_PICO_H
#define TNFS_PICO_H

void tnfs_pico_init(void);
void tnfs_pico_poll(void);   /* boucle principale, après tud_task() */

#endif
