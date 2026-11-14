#ifndef SYSTEM_H
#define SYSTEM_H
#include <stddef.h>
#include <stdint.h>

/* The running application image as mapped at 0x10000000, rounded up to whole UF2 pages. */
const uint8_t *system_image(size_t *size);
/* Reboots into the uf2loader SD menu; does not return on the device. */
void system_reboot_to_loader(void);
#endif
