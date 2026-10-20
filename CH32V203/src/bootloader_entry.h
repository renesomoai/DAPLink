#ifndef BOOTLOADER_ENTRY_H
#define BOOTLOADER_ENTRY_H
#include <stdint.h>

/* Reboot into the WCH USB ISP bootloader (VID 4348 PID 55E0) so the next flash needs no BOOT button.
 * Triggers: DAP vendor command 0x82 [variant] and a 1200-baud "touch" of the CDC port (Arduino convention).
 * The request is only recorded; the reboot happens from the main loop ~100 ms later so the host gets its answer first.
 * The physical BOOT button remains the recovery path (hung firmware). */
void bootloader_request(uint8_t variant);
void bootloader_poll(void);                  /* call from the main loop */

#define BOOTLOADER_VARIANT_BOOTMODE  0       /* FLASH BOOT_MODE bit (unlocked with BOOT_MODEKEYR) + system reset */
#define BOOTLOADER_VARIANT_JUMP      1       /* clean peripherals, jump to the system bootloader at 0x1FFF8000 */

#endif
