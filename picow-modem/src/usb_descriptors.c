/* usb_descriptors.c — descripteurs USB : périphérique CDC-ACM « Neo6502 Wi-Fi modem ». */
#include "tusb.h"
#include "pico/unique_id.h"

/* VID/PID de test Raspberry Pi (0x2E8A, PID 0x000A = « Pico SDK CDC ») */
#define USB_VID 0x2E8A
#define USB_PID 0x000A

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *string_desc[] = {
    (const char[]){ 0x09, 0x04 },   /* 0 : langue, anglais (0x0409) */
    "Neo6502drive",                  /* 1 : fabricant */
    "Pico W Wi-Fi modem",            /* 2 : produit */
    NULL,                            /* 3 : numéro de série (identifiant flash) */
    "Modem AT",                      /* 4 : interface CDC */
};

static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t len;
    if (index == 0) {
        memcpy(&desc_str[1], string_desc[0], 2);
        len = 1;
    } else {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        const char *s;
        if (index == 3) { pico_get_unique_board_id_string(serial, sizeof serial); s = serial; }
        else if (index < sizeof string_desc / sizeof string_desc[0]) s = string_desc[index];
        else return NULL;
        len = (uint8_t)strlen(s);
        if (len > 31) len = 31;
        for (uint8_t i = 0; i < len; i++) desc_str[1 + i] = (uint16_t)s[i];
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
