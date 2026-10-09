/*
 * CMSIS-Driver USART (receive-only subset) for the SWO UART capture of CMSIS-DAP (DAP/Source/SWO.c).
 *
 *   SWO pin : PA10 = USART1_RX (input with pull-up: SWO is NRZ, idle high)
 *   DMA     : DMA1 channel 5 (USART1_RX), normal mode, one block per Receive() call (SWO.c re-arms it from
 *             the completion callback, 1..64 byte blocks)
 *   Clock   : USART1 is on APB2, which SystemInit leaves undivided: PCLK2 = SystemCoreClock
 *   Baud    : BRR = round(PCLK2 / baud) (16x oversampling); actual rate reported back to the host
 *
 * Interrupts (priority 0, above the USB interrupt so a 64 B block rollover is never held up by USB):
 *   DMA1_Channel5 -> ARM_USART_EVENT_RECEIVE_COMPLETE
 *   USART1 (EIE)  -> ARM_USART_EVENT_RX_OVERFLOW / RX_FRAMING_ERROR
 * The interrupt bodies are plain functions (swo_dma_isr / swo_usart_isr) so that the virtual bench can call them.
 *
 * Limits: between two blocks the DMA is idle for a few hundred ns (ISR + re-arm); a byte arriving in that gap is an
 * overrun (reported as DAP_SWO_BUFFER_OVERRUN). Keep the SWO baud rate moderate (<= 2-3 Mbaud) until measured.
 */
#include <stdint.h>
#include "ch32v20x.h"
#include "ch32v20x_usart.h"
#include "ch32v20x_dma.h"
#include "ch32v20x_gpio.h"
#include "ch32v20x_rcc.h"
#include "ch32v20x_misc.h"
#include "Driver_USART.h"
#include "irq_attr.h"

#define SWO_DMA_CH      DMA1_Channel5
#define SWO_DMA_TC      DMA1_IT_TC5
#define SWO_DMA_GL      DMA1_IT_GL5
#define SWO_DMA_TE      DMA1_IT_TE5

extern uint32_t SystemCoreClock;

static ARM_USART_SignalEvent_t cb_event;
static uint8_t           *rx_buf;
static uint32_t           rx_len;
static volatile uint32_t  rx_done;
static volatile uint8_t   rx_busy;
static volatile uint8_t   st_overflow, st_framing;
static uint8_t            initialized;

/* ---------------------------------------------------------------------------------------------------------- */
static ARM_DRIVER_VERSION GetVersion(void)
{
    ARM_DRIVER_VERSION v = { ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0), ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0) };
    return v;
}

static ARM_USART_CAPABILITIES GetCapabilities(void)
{
    ARM_USART_CAPABILITIES c = {0};
    c.asynchronous = 1;
    return c;
}

static int32_t Initialize(ARM_USART_SignalEvent_t cb)
{
    GPIO_InitTypeDef gpio;
    NVIC_InitTypeDef nvic;

    cb_event = cb;
    rx_busy = 0;
    rx_done = 0;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO | RCC_APB2Periph_USART1, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    gpio.GPIO_Pin   = GPIO_Pin_10;
    gpio.GPIO_Mode  = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);                              /* PA10 = USART1_RX */

    nvic.NVIC_IRQChannel = DMA1_Channel5_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 0;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannel = USART1_IRQn;
    NVIC_Init(&nvic);

    initialized = 1;
    return ARM_DRIVER_OK;
}

static int32_t Uninitialize(void)
{
    NVIC_DisableIRQ(DMA1_Channel5_IRQn);
    NVIC_DisableIRQ(USART1_IRQn);
    initialized = 0;
    cb_event = 0;
    return ARM_DRIVER_OK;
}

static int32_t PowerControl(ARM_POWER_STATE state)
{
    switch (state) {
        case ARM_POWER_OFF:
            SWO_DMA_CH->CFGR &= ~DMA_CFGR1_EN;
            USART1->CTLR1 = 0;
            USART1->CTLR3 = 0;
            rx_busy = 0;
            return ARM_DRIVER_OK;
        case ARM_POWER_FULL:
            if (!initialized) { return ARM_DRIVER_ERROR; }
            USART1->CTLR1 = 0;                            /* 8 data bits, no parity, receiver off until Control(RX) */
            USART1->CTLR2 = 0;                            /* 1 stop bit */
            USART1->CTLR3 = USART_CTLR3_DMAR | USART_CTLR3_EIE;
            return ARM_DRIVER_OK;
        default:
            return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static int32_t Send(const void *data, uint32_t num)               { (void)data; (void)num; return ARM_DRIVER_ERROR_UNSUPPORTED; }
static int32_t Transfer(const void *o, void *i, uint32_t num)     { (void)o; (void)i; (void)num; return ARM_DRIVER_ERROR_UNSUPPORTED; }
static uint32_t GetTxCount(void)                                  { return 0; }

static int32_t Receive(void *data, uint32_t num)
{
    if ((data == 0) || (num == 0)) { return ARM_DRIVER_ERROR_PARAMETER; }
    if (rx_busy)                   { return ARM_DRIVER_ERROR_BUSY; }

    rx_buf  = (uint8_t *)data;
    rx_len  = num;
    rx_done = 0;
    rx_busy = 1;

    SWO_DMA_CH->CFGR &= ~DMA_CFGR1_EN;
    SWO_DMA_CH->PADDR = (uint32_t)&USART1->DATAR;
    SWO_DMA_CH->MADDR = (uint32_t)data;
    SWO_DMA_CH->CNTR  = num;
    DMA1->INTFCR      = SWO_DMA_GL;                       /* clear all flags of channel 5 */
    SWO_DMA_CH->CFGR  = DMA_CFGR1_MINC | DMA_CFGR1_PL_1 | DMA_CFGR1_TCIE | DMA_CFGR1_TEIE;  /* periph -> mem, byte, normal */
    SWO_DMA_CH->CFGR |= DMA_CFGR1_EN;
    return ARM_DRIVER_OK;
}

static uint32_t GetRxCount(void)
{
    return rx_busy ? (rx_len - SWO_DMA_CH->CNTR) : rx_done;
}

static void abort_receive(void)
{
    if (rx_busy) {
        SWO_DMA_CH->CFGR &= ~DMA_CFGR1_EN;
        rx_done = rx_len - SWO_DMA_CH->CNTR;
        rx_busy = 0;
    }
}

static int32_t Control(uint32_t control, uint32_t arg)
{
    switch (control & ARM_USART_CONTROL_Msk) {
        case ARM_USART_MODE_ASYNCHRONOUS: {
            uint32_t div;
            if (((control & ARM_USART_DATA_BITS_Msk)   != ARM_USART_DATA_BITS_8)   ||
                ((control & ARM_USART_PARITY_Msk)      != ARM_USART_PARITY_NONE)   ||
                ((control & ARM_USART_STOP_BITS_Msk)   != ARM_USART_STOP_BITS_1)   ||
                ((control & ARM_USART_FLOW_CONTROL_Msk) != ARM_USART_FLOW_CONTROL_NONE)) {
                return ARM_DRIVER_ERROR_UNSUPPORTED;
            }
            if (arg == 0U) { return ARM_DRIVER_ERROR_PARAMETER; }
            div = (SystemCoreClock + (arg / 2U)) / arg;    /* PCLK2 / baud, rounded */
            if ((div < 16U) || (div > 0xFFFFU)) { return ARM_DRIVER_ERROR_PARAMETER; }
            USART1->BRR = (uint16_t)div;
            USART1->CTLR1 |= USART_CTLR1_UE;
            return ARM_DRIVER_OK;
        }
        case ARM_USART_CONTROL_RX:
            if (arg) {
                (void)USART1->STATR;                       /* clear stale error flags, drop a stale byte */
                (void)USART1->DATAR;
                USART1->CTLR1 |= USART_CTLR1_RE | USART_CTLR1_UE;
            } else {
                USART1->CTLR1 &= ~USART_CTLR1_RE;
            }
            return ARM_DRIVER_OK;
        case ARM_USART_ABORT_RECEIVE:
            abort_receive();
            return ARM_DRIVER_OK;
        default:
            return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static ARM_USART_STATUS GetStatus(void)
{
    ARM_USART_STATUS s = {0};
    s.rx_busy = rx_busy;
    s.rx_overflow = st_overflow;
    s.rx_framing_error = st_framing;
    return s;
}

static int32_t SetModemControl(ARM_USART_MODEM_CONTROL c) { (void)c; return ARM_DRIVER_ERROR_UNSUPPORTED; }
static ARM_USART_MODEM_STATUS GetModemStatus(void)        { ARM_USART_MODEM_STATUS m = {0}; return m; }

ARM_DRIVER_USART Driver_USART0 = {
    GetVersion, GetCapabilities, Initialize, Uninitialize, PowerControl,
    Send, Receive, Transfer, GetTxCount, GetRxCount, Control, GetStatus,
    SetModemControl, GetModemStatus
};

/* ---------------------------------------------------------------------------------------------------------- */
/* Interrupt bodies                                                                                            */
/* ---------------------------------------------------------------------------------------------------------- */
void swo_dma_isr(void)
{
    uint32_t ev = 0;

    if (DMA1->INTFR & SWO_DMA_TC) {
        DMA1->INTFCR = SWO_DMA_GL;
        rx_done = rx_len;
        rx_busy = 0;
        ev |= ARM_USART_EVENT_RECEIVE_COMPLETE;
    } else if (DMA1->INTFR & SWO_DMA_TE) {                /* bus error: drop the block */
        DMA1->INTFCR = SWO_DMA_GL;
        abort_receive();
        ev |= ARM_USART_EVENT_RX_OVERFLOW;
    }
    if (ev && cb_event) { cb_event(ev); }
}

void swo_usart_isr(void)
{
    uint32_t st = USART1->STATR;
    uint32_t ev = 0;

    if (st & USART_STATR_ORE) { st_overflow = 1; ev |= ARM_USART_EVENT_RX_OVERFLOW; }
    if (st & USART_STATR_FE)  { st_framing  = 1; ev |= ARM_USART_EVENT_RX_FRAMING_ERROR; }
    if (st & (USART_STATR_ORE | USART_STATR_FE | USART_STATR_NE)) {
        (void)USART1->DATAR;                              /* STATR read + DATAR read clears the flags */
    }
    if (ev && cb_event) { cb_event(ev); }
}

void DMA1_Channel5_IRQHandler(void) WCH_IRQ_FAST;
void DMA1_Channel5_IRQHandler(void) { swo_dma_isr(); }

void USART1_IRQHandler(void) WCH_IRQ_FAST;
void USART1_IRQHandler(void) { swo_usart_isr(); }
