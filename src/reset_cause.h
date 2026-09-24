/* reset_cause.h — cause du dernier redémarrage, pour ATI et la bannière (US-W4).
   Portable (testé sur PC, tests/test_reset_cause.c) : main.c lui passe l'état
   matériel lu au boot.

   Registres scratch du watchdog : 4 à 7 appartiennent au SDK et au bootrom
   (watchdog_enable écrit sa marque dans scratch[4], le bootrom s'en sert pour
   « redémarrer à une adresse ») ; le modem n'utilise que 0 à 2. */
#ifndef RESET_CAUSE_H
#define RESET_CAUSE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RESET_SCRATCH_STAGE  0            /* dernier point d'étape (net_pico_stage)   */
#define RESET_SCRATCH_MSG    1            /* message d'assertion lwIP (chaîne en flash) */
#define RESET_SCRATCH_MARKER 2            /* marqueur posé juste avant un redémarrage  */

#define RESET_MARK_LWIP 0x4C574950u       /* 'LWIP' : assertion lwIP, puis watchdog    */
#define RESET_MARK_RST  0x41525354u       /* 'ARST' : AT+RST                           */
#define RESET_MARK_BSEL 0x4253454Cu       /* 'BSEL' : AT+BOOTSEL (puis copie d'un UF2) */

struct reset_state {
    bool by_watchdog;        /* watchdog_caused_reboot() : tout reset passant par le watchdog */
    bool watchdog_timeout;   /* watchdog_enable_caused_reboot() : dépassement réel             */
    uint32_t marker, stage;
    const char *lwip_msg;    /* NULL si inconnu */
};

/* Écrit « last reset: … » dans out. */
void reset_cause_format(const struct reset_state *st, char *out, size_t n);

#endif
