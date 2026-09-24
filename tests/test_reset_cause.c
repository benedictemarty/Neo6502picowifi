/* test_reset_cause.c — cause du dernier redémarrage (US-W4) : chaque cas, et
   les priorités (assertion lwIP > dépassement du watchdog > marqueurs). */
#include "../src/reset_cause.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;

static void expect(struct reset_state st, const char *want, int line)
{
    char out[96];
    reset_cause_format(&st, out, sizeof out);
    checks++;
    if (strcmp(out, want)) { failures++; printf("ÉCHEC l.%d : « %s », attendu « %s »\n", line, out, want); }
}
#define EXPECT(st, want) expect(st, want, __LINE__)

int main(void)
{
    /* mise sous tension : les marqueurs résiduels sont ignorés */
    EXPECT(((struct reset_state){ 0 }), "last reset: power-on");
    EXPECT(((struct reset_state){ .marker = RESET_MARK_RST, .stage = 12 }), "last reset: power-on");

    /* le constat de US-W4 : reboot du bootrom après copie d'un UF2 (BOOTSEL au bouton) */
    EXPECT(((struct reset_state){ .by_watchdog = true }), "last reset: reboot (bootloader or debugger)");
    EXPECT(((struct reset_state){ .by_watchdog = true, .stage = 3 }), "last reset: reboot (bootloader or debugger)");

    /* vrais dépassements */
    EXPECT(((struct reset_state){ .by_watchdog = true, .watchdog_timeout = true, .stage = 12 }),
           "last reset: watchdog timeout, stage 12");
    EXPECT(((struct reset_state){ .by_watchdog = true, .watchdog_timeout = true, .marker = RESET_MARK_BSEL, .stage = 0 }),
           "last reset: watchdog timeout, stage 0");

    /* assertion lwIP : prioritaire (elle attend justement le dépassement) */
    EXPECT(((struct reset_state){ .by_watchdog = true, .watchdog_timeout = true, .marker = RESET_MARK_LWIP,
                                  .lwip_msg = "pbuf_free: p->ref > 0" }),
           "last reset: lwip assert: pbuf_free: p->ref > 0");
    EXPECT(((struct reset_state){ .by_watchdog = true, .marker = RESET_MARK_LWIP }), "last reset: lwip assert: ?");

    /* redémarrages demandés */
    EXPECT(((struct reset_state){ .by_watchdog = true, .marker = RESET_MARK_RST }), "last reset: AT+RST");
    EXPECT(((struct reset_state){ .by_watchdog = true, .marker = RESET_MARK_BSEL }), "last reset: AT+BOOTSEL (UF2 flash)");
    EXPECT(((struct reset_state){ .by_watchdog = true, .marker = 0x12345678u }), "last reset: reboot (bootloader or debugger)");

    /* message tronqué à 60 caractères, tampon court respecté */
    char msg[100];
    memset(msg, 'x', 99); msg[99] = 0;
    char want[96];
    snprintf(want, sizeof want, "last reset: lwip assert: %.60s", msg);
    EXPECT(((struct reset_state){ .by_watchdog = true, .marker = RESET_MARK_LWIP, .lwip_msg = msg }), want);
    char small[8];
    struct reset_state st = { .by_watchdog = true, .marker = RESET_MARK_RST };
    reset_cause_format(&st, small, sizeof small);
    checks++;
    if (strcmp(small, "last re")) { failures++; printf("ÉCHEC tampon court : « %s »\n", small); }

    /* registres : jamais ceux du SDK / bootrom (4 à 7) */
    checks++;
    if (RESET_SCRATCH_STAGE > 3 || RESET_SCRATCH_MSG > 3 || RESET_SCRATCH_MARKER > 3) { failures++; printf("ÉCHEC registres scratch\n"); }

    printf("%d vérifications, %d échec(s)\n", checks, failures);
    return failures ? 1 : 0;
}
