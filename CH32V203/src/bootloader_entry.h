#ifndef BOOTLOADER_ENTRY_H
#define BOOTLOADER_ENTRY_H
#include <stdint.h>

/* Reboot into the WCH USB ISP bootloader (VID 4348 PID 55E0) without the BOOT button.
 *
 * Hardware (T1 bodge, docs/22): PB15 (U1.28) -> 1 kOhm -> BOOT0 (TP11), 4.7 uF BOOT0 -> GND; optional wire PB14 (U1.27) -> NRST (TP10).
 * The firmware charges the BOOT0 capacitor through PB15, then resets; the capacitor keeps BOOT0 high for ~50 ms after the
 * reset has floated the GPIO, which is when the ROM samples it. Without that hardware the request is harmless: the chip just
 * reboots into the application again.
 *
 * Trigger: DAP vendor command 0x82 [method], answered immediately; the reboot runs from the main loop ~100 ms later.
 *   method 0: system reset (NVIC_SystemReset)
 *   method 1: hardware reset: PB14 pulls NRST low (falls back to a system reset if the wire is absent) */
void bootloader_request(uint8_t method);
void bootloader_poll(void);                  /* call from the main loop */

#endif
