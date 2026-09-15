/* tls_date.h — date civile depuis l'époque Unix, sans gmtime_r (newlib prend
   un verrou : interdit dans le contexte lwIP). Portable, testé sur PC. */
#ifndef TLS_DATE_H
#define TLS_DATE_H
#include <time.h>
/* out = {année, mois 1-12, jour 1-31, heure, minute, seconde} (UTC) */
void civil_from_epoch(time_t t, int out[6]);
/* -1, 0, 1 : comparaison lexicographique de deux dates civiles */
int cmp6(const int a[6], const int b[6]);
#endif
