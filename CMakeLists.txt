cmake_minimum_required(VERSION 3.20)
set(PICO_BOARD pico2_w CACHE STRING "Target board")
if(NOT PICO_BOARD STREQUAL "pico2_w")
    message(FATAL_ERROR "This firmware targets the Raspberry Pi Pico 2 W")
endif()
if(NOT DEFINED PICO_SDK_PATH AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.deps/pico-sdk")
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/.deps/pico-sdk")
endif()
include(pico_sdk_import.cmake)
project(picocalc_ssh C CXX ASM)
# VERSION holds MAJOR.MINOR; CI release builds pass the full -DAPP_VERSION=MAJOR.MINOR.PATCH.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" app_version_base LIMIT_COUNT 1)
set(APP_VERSION "${app_version_base}.0-dev" CACHE STRING "Firmware version shown on the Settings page")
if(NOT APP_VERSION MATCHES "^[0-9A-Za-z._+-]+$")
    message(FATAL_ERROR "APP_VERSION may only contain letters, digits, '.', '_', '+' and '-'")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

include(tools/crypto.cmake)
add_executable(picocalc_ssh
    src/main.c src/app.c src/model.c src/storage.c src/network.c src/ssh_client.c
    src/terminal.c src/log.c src/random.c
    src/hw/lcd.c src/hw/keyboard.c src/hw/sdcard.c
    src/ui/gfx.c src/ui/font_6x12.c
    third_party/fatfs/ff.c third_party/fatfs/ffunicode.c)
target_include_directories(picocalc_ssh PRIVATE src third_party/fatfs)
target_compile_definitions(picocalc_ssh PRIVATE
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0 PICO_MALLOC_PANIC=0 PICO_STACK_SIZE=16384
    CYW43_HOST_NAME="picocalc-ssh" APP_VERSION="${APP_VERSION}")
target_compile_options(picocalc_ssh PRIVATE -Wall -Wextra -Werror
    -Wno-unused-parameter -Wno-format-truncation)
set_source_files_properties(third_party/fatfs/ff.c third_party/fatfs/ffunicode.c
    PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(picocalc_ssh PRIVATE ssh_crypto pico_stdlib pico_rand
    hardware_spi hardware_i2c hardware_dma pico_cyw43_arch_lwip_poll)
pico_enable_stdio_usb(picocalc_ssh 1)
pico_enable_stdio_uart(picocalc_ssh 0)
pico_set_program_name(picocalc_ssh "PicoCalc SSH")
pico_set_program_version(picocalc_ssh "${APP_VERSION}")
# SSH's PTY and crypto call stacks exceed the SDK's 4 KiB scratch bank.
# Reserve 16 KiB at the top of main SRAM; leave the UF2 flash layout unchanged.
file(READ "${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld" ssh_linker)
if(NOT ssh_linker MATCHES "ORIGIN =  0x20000000, LENGTH = 512k" OR
   NOT ssh_linker MATCHES "ORIGIN = 0x20081000, LENGTH = 4k")
    message(FATAL_ERROR "Unexpected SDK memory layout; use the pinned Pico SDK 2.2.0")
endif()
string(REPLACE "ORIGIN =  0x20000000, LENGTH = 512k"
    "ORIGIN =  0x20000000, LENGTH = 496k" ssh_linker "${ssh_linker}")
string(REPLACE "ORIGIN = 0x20081000, LENGTH = 4k"
    "ORIGIN = 0x2007c000, LENGTH = 16k" ssh_linker "${ssh_linker}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ssh_memmap.ld" "${ssh_linker}")
pico_set_linker_script(picocalc_ssh "${CMAKE_CURRENT_BINARY_DIR}/ssh_memmap.ld")
pico_add_extra_outputs(picocalc_ssh)
