/*
 * Test Domain Timer for CMSIS-DAP (TIMESTAMP_GET), built on the 1 ms SysTick tick of main.c.
 *
 * The template shipped a stub that always returned 0 while advertising TIMESTAMP_CLOCK, so every
 * "wait up to N us" loop in DAP.c (DAP_SWJ_Pins) could never time out: the probe hung forever when a
 * pin did not reach the requested level (e.g. nRESET held low by the target or a supervisor).
 *
 * SysTick runs on HCLK, counts 0..CMP with auto-reload every ms, and SysTick_ms is incremented in its
 * interrupt: time = SysTick_ms * (HCLK/1000) + CNT. The result never goes backwards and wraps modulo 2^32
 * (29.8 s at 144 MHz), which is fine for the 3 s maximum wait of DAP_SWJ_Pins (unsigned differences).
 */
#include "ch32v20x.h"
#include "DAP_config.h"

extern volatile uint32_t SysTick_ms;

uint32_t TIMESTAMP_GET(void)
{
    static uint32_t last;
    uint32_t ms, sr, cnt, t;

    do {
        ms  = SysTick_ms;
        sr  = SysTick->SR;
        cnt = *(volatile uint32_t *)&SysTick->CNT;
    } while (ms != SysTick_ms);

    if (sr & 1U) {
        ms++;                              /* compare match latched, its interrupt has not run yet */
    }
    t = ms * (TIMESTAMP_CLOCK / 1000U) + cnt;

    if ((int32_t)(t - last) < 0) {         /* rare window inside the SysTick ISR: never return a smaller value */
        t = last;
    }
    last = t;
    return t;
}
