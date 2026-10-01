/* test_tnfs_link.c — trames du port USB TNFS (longueur 2 octets petit-boutiste) : tests sur PC. */
#include "../src/tnfs_link.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint8_t got[8][TNFS_DGRAM_MAX];
static size_t got_len[8];
static int got_n;

static void cb(void *ctx, const uint8_t *d, size_t len)
{
    (void)ctx;
    if (got_n < 8) { memcpy(got[got_n], d, len); got_len[got_n] = len; }
    got_n++;
}

static size_t frame(uint8_t *f, const uint8_t *d, size_t len)
{
    f[0] = (uint8_t)len; f[1] = (uint8_t)(len >> 8);
    memcpy(f + 2, d, len);
    return len + 2;
}

static void test_rx(void)
{
    struct tnfs_rx r;
    tnfs_rx_init(&r);
    uint8_t f[4096], big[TNFS_DGRAM_MAX];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7);

    /* deux trames dans un même bloc */
    size_t n = frame(f, (const uint8_t *)"abc", 3);
    n += frame(f + n, big, 532);
    got_n = 0;
    tnfs_rx_feed(&r, f, n, cb, NULL);
    CHECK(got_n == 2 && got_len[0] == 3 && !memcmp(got[0], "abc", 3));
    CHECK(got_len[1] == 532 && !memcmp(got[1], big, 532));

    /* trame maximale découpée octet par octet */
    n = frame(f, big, TNFS_DGRAM_MAX);
    got_n = 0;
    for (size_t i = 0; i < n; i++) tnfs_rx_feed(&r, f + i, 1, cb, NULL);
    CHECK(got_n == 1 && got_len[0] == TNFS_DGRAM_MAX && !memcmp(got[0], big, TNFS_DGRAM_MAX));

    /* longueur invalide : resynchronisation, la trame suivante passe */
    uint8_t bad[4] = { 0xff, 0xff, 1, 2 };
    got_n = 0;
    tnfs_rx_feed(&r, bad, sizeof bad, cb, NULL);
    CHECK(got_n == 0 && r.resyncs == 1 && r.n == 0);
    uint8_t zero[2] = { 0, 0 };
    tnfs_rx_feed(&r, zero, 2, cb, NULL);
    CHECK(r.resyncs == 2);
    uint8_t over[2] = { (uint8_t)(TNFS_DGRAM_MAX + 1), (uint8_t)((TNFS_DGRAM_MAX + 1) >> 8) };
    tnfs_rx_feed(&r, over, 2, cb, NULL);
    CHECK(r.resyncs == 3);
    n = frame(f, (const uint8_t *)"ok", 2);
    tnfs_rx_feed(&r, f, n, cb, NULL);
    CHECK(got_n == 1 && got_len[0] == 2 && !memcmp(got[0], "ok", 2));
}

static void test_queue(void)
{
    static struct tnfs_queue q;
    tnfs_queue_init(&q);
    uint8_t d[TNFS_DGRAM_MAX], out[2 + TNFS_DGRAM_MAX];
    for (size_t i = 0; i < sizeof d; i++) d[i] = (uint8_t)i;
    CHECK(tnfs_queue_peek(&q) == 0);
    CHECK(tnfs_queue_push(&q, d, 532));
    CHECK(tnfs_queue_push(&q, d, 1));
    CHECK(tnfs_queue_peek(&q) == 534);
    CHECK(tnfs_queue_pop(&q, out, 100) == 0);                 /* destination trop petite */
    CHECK(tnfs_queue_pop(&q, out, sizeof out) == 534);
    CHECK(out[0] == (532 & 0xff) && out[1] == (532 >> 8) && !memcmp(out + 2, d, 532));
    CHECK(tnfs_queue_pop(&q, out, sizeof out) == 3 && out[0] == 1 && out[1] == 0 && out[2] == 0);
    CHECK(tnfs_queue_peek(&q) == 0);
    /* tailles refusées */
    CHECK(!tnfs_queue_push(&q, d, 0) && !tnfs_queue_push(&q, d, TNFS_DGRAM_MAX + 1) && q.dropped == 2);
    /* file pleine : tout ou rien, les trames déjà rangées restent intactes ; tour du tampon */
    int pushed = 0;
    while (tnfs_queue_push(&q, d, TNFS_DGRAM_MAX)) pushed++;
    CHECK(pushed == (TNFS_QUEUE_SIZE - 1) / (TNFS_DGRAM_MAX + 2) && q.dropped == 3);
    for (int i = 0; i < pushed; i++) {
        CHECK(tnfs_queue_pop(&q, out, sizeof out) == 2 + TNFS_DGRAM_MAX && !memcmp(out + 2, d, TNFS_DGRAM_MAX));
        CHECK(tnfs_queue_push(&q, d, 100 + (size_t)i));       /* écrit à cheval sur la fin du tampon */
    }
    for (int i = 0; i < pushed; i++)
        CHECK(tnfs_queue_pop(&q, out, sizeof out) == 2 + 100 + (size_t)i && !memcmp(out + 2, d, 100 + (size_t)i));
    tnfs_queue_push(&q, d, 10);
    tnfs_queue_clear(&q);
    CHECK(tnfs_queue_peek(&q) == 0);
}

int main(void)
{
    test_rx();
    test_queue();
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
