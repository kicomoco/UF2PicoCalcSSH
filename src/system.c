#include "system.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

/* uf2loader stage3 command protocol (common/bootloader/proginfo.h). */
#define PICOCALC_BL_MAGIC 0xe98cc638u
#define PICOCALC_BOOT_SD 1u

extern char __flash_binary_start, __flash_binary_end;

const uint8_t *system_image(size_t *size) {
    uintptr_t start = (uintptr_t)&__flash_binary_start, end = (uintptr_t)&__flash_binary_end;
    *size = (end - start + 255u) & ~(size_t)255u;
    return (const uint8_t *)start;
}

void system_reboot_to_loader(void) {
    watchdog_hw->scratch[1] = PICOCALC_BOOT_SD;
    watchdog_hw->scratch[2] = 0;
    watchdog_hw->scratch[0] = PICOCALC_BL_MAGIC;
    watchdog_reboot(0, 0, 10);
    while (true) tight_loop_contents();
}
