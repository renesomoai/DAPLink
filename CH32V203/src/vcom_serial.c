#include <string.h>
#include "ch32v20x.h"
#include "usb_lib.h"
#include "usb_regs.h"
#include "usb_desc.h"
#include "vcom_serial.h"
#include "irq_attr.h"


volatile VCOM Vcom;

static volatile uint32_t rx_wraps;           /* completed laps of the circular RX DMA (DMA TC interrupt) */
static uint32_t rx_consumed;                 /* total bytes handed to USB (monotonic) */
uint32_t vcom_rx_overruns;                   /* bytes lost because the ring was not drained in time */
uint32_t vcom_rx_total;                      /* bytes delivered to the host */

VCOM_LINE_CODING LineCfg = {115200, 0, 0, 8};   // Baud rate, stop bits, parity bits, data bits


/* RX ring: USART2 -> DMA (circular). It is drained from the main loop, which a DAP command can occupy for several ms
 * (measured on hardware: 1.4-6 ms at 1-8 MHz SWCLK, more at slow clocks); the old 128-byte ring overran at 921600 baud
 * whenever the DAP was busy. 2 KB gives 22 ms of headroom at 921600 baud and 178 ms at 115200. */
#define RXDMA_SZ  2048U
uint8_t RXBuffer[RXDMA_SZ] __attribute__((aligned(4)));
uint8_t TXBuffer[CDC_BULK_OUT_SZ] __attribute__((aligned(4)));


void VCOM_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    DMA_InitTypeDef   DMA_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_AHBPeriphClockCmd (RCC_AHBPeriph_DMA1, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);              // PA2 => USART2_TX

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);              // PA3 => USART2_RX


    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&USART2->DATAR;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryBaseAddr = (u32)TXBuffer;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_BufferSize = 0;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel7, &DMA_InitStructure);
    DMA_Cmd(DMA1_Channel7, ENABLE);

    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_PeripheralBaseAddr = (u32)&USART2->DATAR;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryBaseAddr = (u32)RXBuffer;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_BufferSize = RXDMA_SZ;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel6, &DMA_InitStructure);
    DMA_ITConfig(DMA1_Channel6, DMA_IT_TC, ENABLE);
    DMA_Cmd(DMA1_Channel6, ENABLE);
    {
        NVIC_InitTypeDef nvic;
        nvic.NVIC_IRQChannel = DMA1_Channel6_IRQn;
        nvic.NVIC_IRQChannelPreemptionPriority = 0;
        nvic.NVIC_IRQChannelSubPriority = 0;
        nvic.NVIC_IRQChannelCmd = ENABLE;
        NVIC_Init(&nvic);
    }


    USART_InitStructure.USART_BaudRate = 115200;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &USART_InitStructure);

    USART_DMACmd(USART2, USART_DMAReq_Tx | USART_DMAReq_Rx, ENABLE);

    USART_Cmd(USART2, ENABLE);
}


void VCOM_LineCoding(VCOM_LINE_CODING * LineCfgx)
{
    USART_InitTypeDef USART_InitStructure;
    
    switch(LineCfgx->u8DataBits)
    {
    case 8:  USART_InitStructure.USART_WordLength = USART_WordLength_8b; break;
    default: USART_InitStructure.USART_WordLength = USART_WordLength_8b; break;
    }
    
    switch(LineCfgx->u8ParityType)
    {
    case 0:  USART_InitStructure.USART_Parity     = USART_Parity_No;     break;
    case 1:  USART_InitStructure.USART_Parity     = USART_Parity_Odd;
             USART_InitStructure.USART_WordLength = USART_WordLength_9b; break;
    case 2:  USART_InitStructure.USART_Parity     = USART_Parity_Even;
             USART_InitStructure.USART_WordLength = USART_WordLength_9b; break;
    default: USART_InitStructure.USART_Parity     = USART_Parity_No;     break;
    }

    switch(LineCfgx->u8CharFormat)
    {
    case 0:  USART_InitStructure.USART_StopBits = USART_StopBits_1;   break;
    case 1:  USART_InitStructure.USART_StopBits = USART_StopBits_1_5; break;
    case 2:  USART_InitStructure.USART_StopBits = USART_StopBits_2;   break;
    default: USART_InitStructure.USART_StopBits = USART_StopBits_1;   break;
    }

    USART_InitStructure.USART_BaudRate = LineCfgx->u32DTERate;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;


    __disable_irq();
    
    USART_Init(USART2, &USART_InitStructure);
    
    __enable_irq();
}


extern volatile uint32_t SysTick_ms;

/* Total bytes the DMA has written since start (monotonic), consistent with the wrap counter. */
static uint32_t rx_total_written(void)
{
    uint32_t w, pos;

    __disable_irq();
    w = rx_wraps;
    pos = RXDMA_SZ - DMA_GetCurrDataCounter(DMA1_Channel6);
    if (DMA1->INTFR & DMA1_IT_TC6) {          /* lap finished, its interrupt is still pending (IRQs are masked here) */
        w++;
        pos = RXDMA_SZ - DMA_GetCurrDataCounter(DMA1_Channel6);
    }
    __enable_irq();
    return w * RXDMA_SZ + pos;
}

void DMA1_Channel6_IRQHandler(void) WCH_IRQ_FAST;
void DMA1_Channel6_IRQHandler(void)
{
    rx_wraps++;
    DMA1->INTFCR = DMA1_IT_GL6;
}

uint32_t vcom_stats_get(uint8_t *out)
{
    uint32_t v[2], i;
    v[0] = vcom_rx_total; v[1] = vcom_rx_overruns;
    for (i = 0; i < 2U; i++) { out[4*i] = (uint8_t)v[i]; out[4*i+1] = (uint8_t)(v[i] >> 8); out[4*i+2] = (uint8_t)(v[i] >> 16); out[4*i+3] = (uint8_t)(v[i] >> 24); }
    return 8U;
}

void VCOM_TransferData(void)
{
    static uint32_t last_ms = 0;

    if(GetEPTxStatus(ENDP3) == EP_TX_NAK)
    {
        uint32_t total = rx_total_written();
        uint32_t avail = total - rx_consumed;

        if (avail > RXDMA_SZ - CDC_BULK_IN_SZ)    /* the DMA is about to lap data that was not sent yet: drop the oldest, count it */
        {
            uint32_t drop = avail - (RXDMA_SZ - CDC_BULK_IN_SZ);
            rx_consumed += drop;
            vcom_rx_overruns += drop;
            avail -= drop;
        }

        {
            uint32_t idx = rx_consumed % RXDMA_SZ;
            uint32_t n = avail;
            if (n > CDC_BULK_IN_SZ)    n = CDC_BULK_IN_SZ;
            if (n > RXDMA_SZ - idx)    n = RXDMA_SZ - idx;     /* one contiguous piece per packet */

            if((n >= CDC_BULK_IN_SZ) || ((n != 0U) && (SysTick_ms != last_ms)))
            {
                Vcom.in_bytes = n;

                USB_SIL_Write(EP3_IN, &RXBuffer[idx], n);
                SetEPTxValid(ENDP3);

                rx_consumed += n;
                vcom_rx_total += n;
                last_ms = SysTick_ms;
            }
            else
            {
                /* Prepare a zero packet if previous packet size is CDC_BULK_IN_SZ and
                   no more data to send at this moment to note Host the transfer has been done */
                if(Vcom.in_bytes == CDC_BULK_IN_SZ)
                {
                    Vcom.in_bytes = 0;

                    USB_SIL_Write(EP3_IN, (uint8_t *)0, 0);
                    SetEPTxValid(ENDP3);
                }
            }
        }
    }

	/* 从主机接收到数据，且前面的数据 DMA 已发送完 */
    if(Vcom.out_bytes && (DMA_GetCurrDataCounter(DMA1_Channel7) == 0))
    {
        memcpy(TXBuffer, (uint8_t *)Vcom.out_buff, Vcom.out_bytes);

        DMA_Cmd(DMA1_Channel7, DISABLE);
        DMA_SetCurrDataCounter(DMA1_Channel7, Vcom.out_bytes);
        DMA_Cmd(DMA1_Channel7, ENABLE);

        Vcom.out_bytes = 0;

        /* Ready for next BULK OUT */
		SetEPRxValid(ENDP3);
    }
}
