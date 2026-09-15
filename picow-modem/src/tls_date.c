/* tls_date.c — voir tls_date.h. Algorithme « civil_from_days » (ères de 400
   ans), purement entier. */
#include "tls_date.h"

void civil_from_epoch(time_t t, int out[6])
{
    long long days = (long long)(t / 86400), secs = (long long)(t % 86400);
    if (secs < 0) { secs += 86400; days--; }
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    out[0] = (int)(yoe + era * 400 + (m <= 2));
    out[1] = (int)m; out[2] = (int)d;
    out[3] = (int)(secs / 3600); out[4] = (int)(secs % 3600 / 60); out[5] = (int)(secs % 60);
}

int cmp6(const int a[6], const int b[6])
{
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
