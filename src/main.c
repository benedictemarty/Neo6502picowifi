/*
 * main.c — modem Wi-Fi Neo6502drive sur Raspberry Pi Pico W.
 *
 * Deux transports série alimentent le même modem :
 *  - USB CDC-ACM (TinyUSB device) : PC aujourd'hui, Neo6502 après la story
 *    F-13 (hôte CDC) du firmware Neo6502 ;
 *  - UART0 GP0 (TX) / GP1 (RX), 115200 8N1 : connecteur UEXT du Neo6502,
 *    comme le MOD-WIFI-ESP8266 (netsetup.pas : NeoSetupUART(115200)).
 * Les réponses sont émises sur les deux transports. La LED de la carte
 * s'allume quand le Wi-Fi est associé, clignote pendant une connexion TCP.
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "at_modem.h"
#include "net_pico.h"

#define UART_ID     uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#ifndef UART_BAUD
#define UART_BAUD   115200
#endif

static struct at_modem modem;
static char boot_info[96];
static const char *boot_info_op(void *ctx) { (void)ctx; return boot_info; }

/* --------------------------------------------------------- UART RX */

#define UART_RING 1024
static volatile uint8_t uart_ring[UART_RING];
static volatile uint16_t uart_head, uart_tail;

static void on_uart_rx(void)
{
    while (uart_is_readable(UART_ID)) {
        uint32_t dr = uart_get_hw(UART_ID)->dr;
        if (dr & (UART_UARTDR_FE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_PE_BITS)) continue; /* octet erroné */
        uint8_t c = (uint8_t)dr;
        uint16_t next = (uart_head + 1) % UART_RING;
        if (next != uart_tail) { uart_ring[uart_head] = c; uart_head = next; }
    }
}

static size_t uart_drain(uint8_t *dst, size_t max)
{
    size_t n = 0;
    while (n < max && uart_tail != uart_head) {
        dst[n++] = uart_ring[uart_tail];
        uart_tail = (uart_tail + 1) % UART_RING;
    }
    return n;
}

/* ----------------------------------------------------------- sortie */

static void serial_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    uart_write_blocking(UART_ID, data, len);
    if (!tud_cdc_connected()) return;
    while (len) {
        uint32_t n = tud_cdc_write(data, (uint32_t)len);
        data += n;
        len -= n;
        tud_cdc_write_flush();
        if (len) tud_task();                      /* laisse l'hôte vider le FIFO */
    }
}

static uint32_t millis(void *ctx)
{
    (void)ctx;
    return to_ms_since_boot(get_absolute_time());
}

/* ------------------------------------------------------------- main */

int main(void)
{
    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    gpio_pull_up(UART_RX_PIN);       /* RX en l'air (UEXT non câblé) : sinon du bruit précède le premier AT */
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_ID, true);
    sleep_ms(2);
    while (uart_is_readable(UART_ID)) (void)uart_get_hw(UART_ID)->dr; /* parasites d'initialisation */
    uart_get_hw(UART_ID)->rsr = 0xf;                                   /* efface les drapeaux d'erreur */
    irq_set_exclusive_handler(UART0_IRQ, on_uart_rx);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(UART_ID, true, false);

    tusb_init();

    struct at_config cfg;
    config_flash_load(&cfg);
    net_pico_ops.write = serial_write;
    net_pico_ops.millis = millis;
    net_pico_ops.boot_info = boot_info_op;
    at_modem_init(&modem, &net_pico_ops, &cfg);

    bool wifi_ok = net_pico_init(&modem);
    if (!wifi_ok) {
        const char msg[] = "\r\nERROR: cyw43 init failed\r\n";
        uart_write_blocking(UART_ID, (const uint8_t *)msg, sizeof msg - 1);
    }

    /* comme l'ESP8266 : rejoint le dernier réseau enregistré, en tâche de fond
       (tentatives répétées, commandes AT disponibles tout de suite) */
    if (wifi_ok && modem.cfg.ssid[0]) net_pico_background_join(true);
    if (wifi_ok && modem.cfg.listen_port)
        net_pico_ops.tcp_listen(NULL, modem.cfg.listen_port);
    if (watchdog_caused_reboot() && watchdog_hw->scratch[6] == 0x4C574950)
        snprintf(boot_info, sizeof boot_info, "last reset: lwip assert: %.60s",
                 (const char *)watchdog_hw->scratch[5]);
    else if (watchdog_caused_reboot())
        snprintf(boot_info, sizeof boot_info, "last reset: watchdog, stage %lu",
                 (unsigned long)watchdog_hw->scratch[4]);
    else
        strcpy(boot_info, "last reset: power-on");
    watchdog_hw->scratch[6] = 0;
    char banner[64];
    snprintf(banner, sizeof banner, "\r\nready (%s)\r\n", boot_info);
    uart_write_blocking(UART_ID, (const uint8_t *)banner, strlen(banner));
    bool banner_usb = false;   /* réémis sur l'USB à la première ouverture du port */
    net_pico_stage(0);
    /* Un blocage de plus de 8 s dans la boucle principale redémarre la carte. Les
       opérations longues (CWJAP 30 s, CWLAP 15 s, CIPSTART 10 s) rafraîchissent le
       compteur dans leurs boucles d'attente (net_pico.c : net_pico_kick). */
    watchdog_enable(8000, true);

    uint8_t buf[256];
    uint32_t led_ms = 0;
    bool led = false;
    while (1) {
        watchdog_update();
        net_pico_stage(1);
        tud_task();
        if (!banner_usb && tud_cdc_connected()) {
            banner_usb = true;
            serial_write(NULL, (const uint8_t *)banner, strlen(banner));
        }
        size_t n = uart_drain(buf, sizeof buf);
        if (n) at_modem_input(&modem, buf, n);
        if (tud_cdc_available()) {
            n = tud_cdc_read(buf, sizeof buf);
            if (n) at_modem_input(&modem, buf, n);
        }
        net_pico_stage(2);
        at_modem_poll(&modem);
        net_pico_stage(3);
        net_pico_poll();

        uint32_t now = millis(NULL);
        if (wifi_ok && now - led_ms >= 250) {
            led_ms = now;
            bool up = net_pico_ops.wifi_connected(NULL);
            bool tcp = net_pico_ops.tcp_connected(NULL);
            led = tcp ? !led : up;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        }
    }
}
