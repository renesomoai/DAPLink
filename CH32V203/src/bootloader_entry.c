/*
 * Software entry into the CH32V203 factory USB bootloader.
 *
 * Variant 0 (BOOT_MODE): unlock FLASH_BOOT_MODEKEYR (offset 0x28, keys 0x45670123 / 0xCDEF89AB), set STATR.BOOT_MODE
 *   (bit 14) and request a system reset; the ROM then starts the bootloader instead of the user flash.
 *   Register layout taken from the CH32V003/V20x reference manuals as remembered; VERIFIED ON HARDWARE only (see docs/21).
 * Variant 1 (JUMP): release the USB/GPIO state and jump to the bootloader's reset vector at 0x1FFF8000.
 * Whichever variant the hardware test shows to work is the default used by tools/enter_bootloader.py.
 */
#include "ch32v20x.h"
#include "bootloader_entry.h"
#include "ch32v20x_rcc.h"
#include "ch32v20x_flash.h"

extern volatile uint32_t SysTick_ms;
extern void USB_Port_Set(FunctionalState NewState, FunctionalState Pin_In_IPU);

#define FLASH_BOOT_MODEKEYR   (*(volatile uint32_t *)(FLASH_R_BASE + 0x28U))
#define FLASH_STATR_BOOT_MODE (1UL << 14)
#define FLASH_STATR_BOOT_LOCK (1UL << 15)
#define KEY1 0x45670123UL
#define KEY2 0xCDEF89ABUL
#define SYSTEM_BOOTLOADER     0x1FFF8000UL

static volatile uint8_t  pending;
static volatile uint8_t  req_variant;
static volatile uint32_t deadline;

void bootloader_request(uint8_t variant)
{
    req_variant = variant;
    deadline = SysTick_ms + 100U;
    pending = 1U;
}

static void spin_ms(uint32_t ms)
{
    volatile uint32_t n = ms * (SystemCoreClock / 8000U);
    while (n--) { }
}

static void quiesce(void)
{
    uint32_t i;
    SysTick->CTLR = 0;
    for (i = 0; i < 8U; i++) { NVIC->IRER[i] = 0xFFFFFFFFUL; }   /* every PFIC interrupt off */
    RCC_DeInit();                                                /* back to HSI, PLL/HSE off: the ROM expects reset clocks */
    FLASH->ACTLR = 0;                                            /* default flash wait states for the slow clock */
}

static void enter_bootloader(uint8_t variant)
{
    __disable_irq();
    USB_Port_Set(DISABLE, DISABLE);           /* drop the bus: the host sees a disconnect and re-enumerates the ROM bootloader */
    spin_ms(30U);

    if (variant == BOOTLOADER_VARIANT_JUMP) {
        quiesce();
        RCC->APB1PRSTR = 0xFFFFFFFFUL; RCC->APB1PRSTR = 0;     /* peripheral resets (USB, USART2, timers...) */
        RCC->APB2PRSTR = 0xFFFFFFFFUL; RCC->APB2PRSTR = 0;
        __asm volatile("csrw 0x804, zero");                    /* INTSYSCR: hardware stacking/nesting off, as after reset */
        ((void (*)(void))SYSTEM_BOOTLOADER)();
        for (;;) { }
    }

    if (variant == BOOTLOADER_VARIANT_ERASE0) {
        /* Invalidate the application's first flash page so the ROM does not start it, then reset: the ROM stays in ISP.
         * The next flash writes the whole application again. Recovery if this does not work: BOOT button. */
        FLASH_Unlock_Fast();
        FLASH_ErasePage_Fast(0x08000000UL);
        NVIC_SystemReset();
        for (;;) { }
    }

    FLASH_BOOT_MODEKEYR = KEY1;                                /* variant 0: does NOT work on the CH32V203 (tested) */
    FLASH_BOOT_MODEKEYR = KEY2;
    FLASH->STATR |= FLASH_STATR_BOOT_MODE;
    NVIC_SystemReset();
    for (;;) { }
}

void bootloader_poll(void)
{
    if (pending && ((int32_t)(SysTick_ms - deadline) >= 0)) {
        pending = 0U;
        enter_bootloader(req_variant);
    }
}
