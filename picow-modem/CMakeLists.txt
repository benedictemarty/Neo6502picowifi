# Firmware « modem Wi-Fi » Neo6502drive pour Raspberry Pi Pico W.
#   export PICO_SDK_PATH=/chemin/pico-sdk   (SDK 2.x, avec lib/cyw43-driver, lwip, tinyusb)
#   cmake -S . -B build && cmake --build build      → build/picow_modem.uf2
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Carte cible")
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(picow_modem C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(picow_modem
    src/main.c
    src/at_modem.c
    src/net_pico.c
    src/usb_descriptors.c
)
target_include_directories(picow_modem PRIVATE src)
target_compile_definitions(picow_modem PRIVATE
    PICOW_MODEM_VERSION="0.1.0"
    CYW43_HOST_NAME="neo6502-modem"
)
target_compile_options(picow_modem PRIVATE -Wall -Wextra)
target_link_libraries(picow_modem
    pico_stdlib
    pico_unique_id
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_sntp
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
