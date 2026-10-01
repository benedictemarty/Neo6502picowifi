/* tnfs_link.c — voir tnfs_link.h. */
#include "tnfs_link.h"

#include <string.h>

void tnfs_rx_init(struct tnfs_rx *r) { memset(r, 0, sizeof *r); }

void tnfs_rx_feed(struct tnfs_rx *r, const uint8_t *data, size_t len, tnfs_frame_cb cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        r->buf[r->n++] = data[i];
        if (r->n < 2) continue;
        size_t want = (size_t)r->buf[0] | (size_t)r->buf[1] << 8;
        if (want == 0 || want > TNFS_DGRAM_MAX) {
            /* hors format : on jette tout ce qui est en attente de ce bloc */
            r->n = 0;
            r->resyncs++;
            return;
        }
        if (r->n == 2 + want) {
            cb(ctx, r->buf + 2, want);
            r->n = 0;
        }
    }
}

void tnfs_queue_init(struct tnfs_queue *q) { memset(q, 0, sizeof *q); }

static size_t used(const struct tnfs_queue *q)
{
    return (q->head - q->tail + TNFS_QUEUE_SIZE) % TNFS_QUEUE_SIZE;
}

bool tnfs_queue_push(struct tnfs_queue *q, const uint8_t *dgram, size_t len)
{
    if (len == 0 || len > TNFS_DGRAM_MAX || len + 2 > TNFS_QUEUE_SIZE - 1 - used(q)) {
        q->dropped++;
        return false;
    }
    size_t h = q->head;
    q->ring[h] = (uint8_t)len;        h = (h + 1) % TNFS_QUEUE_SIZE;
    q->ring[h] = (uint8_t)(len >> 8); h = (h + 1) % TNFS_QUEUE_SIZE;
    for (size_t i = 0; i < len; i++) { q->ring[h] = dgram[i]; h = (h + 1) % TNFS_QUEUE_SIZE; }
    q->head = h;
    return true;
}

size_t tnfs_queue_peek(const struct tnfs_queue *q)
{
    if (used(q) < 2) return 0;
    size_t t = q->tail;
    size_t len = (size_t)q->ring[t] | (size_t)q->ring[(t + 1) % TNFS_QUEUE_SIZE] << 8;
    return 2 + len;
}

size_t tnfs_queue_pop(struct tnfs_queue *q, uint8_t *dst, size_t cap)
{
    size_t n = tnfs_queue_peek(q);
    if (n == 0 || n > cap) return 0;
    size_t t = q->tail;
    for (size_t i = 0; i < n; i++) { dst[i] = q->ring[t]; t = (t + 1) % TNFS_QUEUE_SIZE; }
    q->tail = t;
    return n;
}

void tnfs_queue_clear(struct tnfs_queue *q) { q->tail = q->head; }
