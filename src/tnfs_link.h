/*
 * tnfs_link.h — trames du second port USB (CDC 1) réservé à TNFS.
 *
 * Format convenu avec reload-emulator et Neo6502TeleStrat (2026-10-01), dans
 * les deux sens : longueur sur 2 octets petit-boutiste, puis le datagramme
 * (1 à TNFS_DGRAM_MAX octets). Une longueur invalide vide le tampon d'entrée
 * (resynchronisation). Portable, testé sur PC (tests/test_tnfs_link.c).
 */
#ifndef TNFS_LINK_H
#define TNFS_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TNFS_DGRAM_MAX   1472
#define TNFS_DEFAULT_PORT 16384
#define TNFS_QUEUE_SIZE  4096    /* datagrammes reçus du serveur, en attente de l'USB */

/* ------------------------------------------------ entrée (USB → UDP) */

typedef void (*tnfs_frame_cb)(void *ctx, const uint8_t *dgram, size_t len);

struct tnfs_rx {
    uint8_t buf[2 + TNFS_DGRAM_MAX];
    size_t  n;
    unsigned resyncs;          /* longueurs invalides vues (diagnostic) */
};

void tnfs_rx_init(struct tnfs_rx *r);
/* Octets lus sur le port ; cb appelé pour chaque trame complète. */
void tnfs_rx_feed(struct tnfs_rx *r, const uint8_t *data, size_t len, tnfs_frame_cb cb, void *ctx);

/* ---------------------------------------- file de sortie (UDP → USB) */

/* Un producteur (rappel lwIP, interruption), un consommateur (boucle
   principale) : comme le tampon RX du modem, la tête n'avance qu'une fois le
   datagramme complet écrit. Trames rangées telles qu'émises sur l'USB. */
struct tnfs_queue {
    uint8_t ring[TNFS_QUEUE_SIZE];
    volatile size_t head, tail;
    volatile unsigned dropped;  /* datagrammes perdus (file pleine ou taille) */
};

void   tnfs_queue_init(struct tnfs_queue *q);
bool   tnfs_queue_push(struct tnfs_queue *q, const uint8_t *dgram, size_t len);
/* Taille de la prochaine trame (longueur comprise), 0 si la file est vide. */
size_t tnfs_queue_peek(const struct tnfs_queue *q);
/* Copie la prochaine trame dans dst (cap >= tnfs_queue_peek) et la retire. */
size_t tnfs_queue_pop(struct tnfs_queue *q, uint8_t *dst, size_t cap);
void   tnfs_queue_clear(struct tnfs_queue *q);

#endif
