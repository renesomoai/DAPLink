#include "ch32v20x.h"
#include "ch32v20x_gpio.h"
#include "ch32v20x_rcc.h"
#include "bootloader_entry.h"

extern volatile uint32_t SysTick_ms;

#define BOOT0_DRIVE_PORT  GPIOB
#define BOOT0_DRIVE_PIN   GPIO_Pin_15      /* U1.28 -> 1k -> BOOT0 */
#define NRST_PULSE_PORT   GPIOB
#define NRST_PULSE_PIN    GPIO_Pin_14      /* U1.27 -> MCU_NRST (optional wire) */
#define CHARGE_MS         25U              /* ~6 time constants of the 1k||10k * 4.7uF charge (4.3 ms) */
#define NRST_LOW_MS       10U

static volatile uint8_t  pending;
static volatile uint8_t  req_method;
static volatile uint32_t deadline;

void bootloader_request(uint8_t method)
{
    req_method = method;
    deadline = SysTick_ms + 100U;
    pending = 1U;
}

static void wait_ms(uint32_t ms)
{
    uint32_t t0 = SysTick_ms;
    while ((uint32_t)(SysTick_ms - t0) < ms) { }
}

static void pin_config(GPIO_TypeDef *port, uint16_t pin, GPIOMode_TypeDef mode)
{
    GPIO_InitTypeDef g;
    g.GPIO_Pin = pin;
    g.GPIO_Mode = mode;
    g.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(port, &g);
}

static void enter_bootloader(uint8_t method)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_SetBits(BOOT0_DRIVE_PORT, BOOT0_DRIVE_PIN);
    pin_config(BOOT0_DRIVE_PORT, BOOT0_DRIVE_PIN, GPIO_Mode_Out_PP);   /* charge the BOOT0 capacitor */
    wait_ms(CHARGE_MS);

    if (method == 1U) {
        GPIO_ResetBits(NRST_PULSE_PORT, NRST_PULSE_PIN);
        pin_config(NRST_PULSE_PORT, NRST_PULSE_PIN, GPIO_Mode_Out_OD); /* pull NRST low: hardware reset (the GPIO floats again with it) */
        wait_ms(NRST_LOW_MS);
        pin_config(NRST_PULSE_PORT, NRST_PULSE_PIN, GPIO_Mode_IN_FLOATING);   /* only reached when the wire is absent */
    }

    __disable_irq();
    NVIC_SystemReset();                    /* method 0, or the fallback of method 1 */
    for (;;) { }
}

void bootloader_poll(void)
{
    if (pending && ((int32_t)(SysTick_ms - deadline) >= 0)) {
        pending = 0U;
        enter_bootloader(req_method);
    }
}
