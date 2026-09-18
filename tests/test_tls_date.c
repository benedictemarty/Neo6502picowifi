#define _POSIX_C_SOURCE 200809L
/* test_tls_date.c — civil_from_epoch contre gmtime_r sur un large échantillon. */
#include "../src/tls_date.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    int failures = 0, checks = 0;
    const time_t samples[] = { 0, 86399, 86400, 951782400 /* 2000-02-29 */, 1433415878 /* 2015-06-04 11:04:38 */,
                               1789512000, 2063622278 /* 2035-06-04 */, 4102444800 /* 2100-01-01 */, 253402300799 /* 9999-12-31 */ };
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        for (int k = -2; k <= 2; k++) {
            time_t t = samples[i] + k * 3600 * 7 + k;
            struct tm tm; gmtime_r(&t, &tm);
            int ref[6] = { tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec }, got[6];
            civil_from_epoch(t, got);
            checks++;
            if (memcmp(ref, got, sizeof ref)) { failures++; printf("ÉCHEC t=%lld : %d-%02d-%02d %02d:%02d:%02d attendu %d-%02d-%02d %02d:%02d:%02d\n", (long long)t, got[0], got[1], got[2], got[3], got[4], got[5], ref[0], ref[1], ref[2], ref[3], ref[4], ref[5]); }
        }
    }
    /* balayage d'un jour sur deux pendant 60 ans */
    for (time_t t = 1000000000; t < 1000000000 + 60LL * 366 * 86400; t += 2 * 86400 + 3601) {
        struct tm tm; gmtime_r(&t, &tm);
        int ref[6] = { tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec }, got[6];
        civil_from_epoch(t, got);
        checks++;
        if (memcmp(ref, got, sizeof ref)) { failures++; if (failures < 5) printf("ÉCHEC t=%lld\n", (long long)t); }
    }
    int a[6] = { 2026, 9, 16, 0, 0, 0 }, b[6] = { 2026, 9, 16, 0, 0, 1 };
    checks += 3;
    if (cmp6(a, b) != -1) failures++;
    if (cmp6(b, a) != 1) failures++;
    if (cmp6(a, a) != 0) failures++;
    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
