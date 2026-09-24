# Firmware « modem Wi-Fi » Neo6502drive pour Raspberry Pi Pico W.
#   export PICO_SDK_PATH=/chemin/pico-sdk   (SDK 2.x, avec lib/cyw43-driver, lwip, tinyusb)
#   cmake -S . -B build && cmake --build build      → build/picow_modem.uf2
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Carte cible")
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(picow_modem C CXX ASM)
set(CMAKE_C_STANDARD 11)
# -O2 et non -O3 (défaut Release du SDK) : avec GCC 14.2.1 pour Cortex-M0+,
# gcm.c de mbedTLS 3.6.2 compilé en -O3 donne un AES-GCM faux (autotest
# mbedtls_gcm_self_test en échec, alerte bad_record_mac des serveurs) —
# constaté sur carte le 2026-09-16, vérifiable avec AT+TLSTEST.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
pico_sdk_init()

# racines de confiance (certs/roots.pem) → magasin DER + index en flash (US-T13)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/roots_gen.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/roots2c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/certs/roots.pem ${CMAKE_CURRENT_BINARY_DIR}/roots_gen.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/certs/roots.pem ${CMAKE_CURRENT_SOURCE_DIR}/tools/roots2c.py
    COMMENT "Génération de roots_gen.c depuis certs/roots.pem"
)

add_executable(picow_modem
    src/main.c
    src/at_modem.c
    src/net_pico.c
    src/tls_date.c
    src/roots_store.c
    src/roots_ca_cb.c
    src/usb_descriptors.c
    ${CMAKE_CURRENT_BINARY_DIR}/roots_gen.c
)
target_include_directories(picow_modem PRIVATE src)
target_compile_definitions(picow_modem PRIVATE
    PICOW_MODEM_VERSION="0.2.0"
    CYW43_HOST_NAME="neo6502-modem"
)
target_compile_options(picow_modem PRIVATE -Wall -Wextra)
target_link_libraries(picow_modem
    pico_stdlib
    pico_unique_id
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_sntp
    pico_lwip_mbedtls
    pico_mbedtls
    hardware_uart
    hardware_flash
    hardware_watchdog
    pico_bootrom
    tinyusb_device
    tinyusb_board
)
# GP0/GP1 sont le canal UEXT : aucune console stdio dessus, ni sur l'USB.
pico_enable_stdio_uart(picow_modem 0)
pico_enable_stdio_usb(picow_modem 0)
pico_add_extra_outputs(picow_modem)
