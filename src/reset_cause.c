/* reset_cause.c — voir reset_cause.h (US-W4). */
#include "reset_cause.h"
#include <stdio.h>

void reset_cause_format(const struct reset_state *st, char *out, size_t n)
{
    if (!st->by_watchdog)                          /* mise sous tension, broche RUN */
        snprintf(out, n, "last reset: power-on");
    else if (st->marker == RESET_MARK_LWIP)
        snprintf(out, n, "last reset: lwip assert: %.60s", st->lwip_msg ? st->lwip_msg : "?");
    else if (st->watchdog_timeout)
        snprintf(out, n, "last reset: watchdog timeout, stage %lu", (unsigned long)st->stage);
    else if (st->marker == RESET_MARK_RST)
        snprintf(out, n, "last reset: AT+RST");
    else if (st->marker == RESET_MARK_BSEL)
        snprintf(out, n, "last reset: AT+BOOTSEL (UF2 flash)");
    else                                           /* reboot du bootrom après un UF2, débogueur */
        snprintf(out, n, "last reset: reboot (bootloader or debugger)");
}
