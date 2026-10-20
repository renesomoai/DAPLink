/*
 * CMSIS-DAP v2 SWO streaming endpoint (transport 2): bulk IN endpoint 0x84, 64-byte packets.
 *
 * DAP/Source/SWO.c decides what to send (SWO_QueueTransfer) and is told when a transfer is done (SWO_TransferComplete);
 * this file moves the bytes through the USB peripheral one packet at a time. Hosts that find a third endpoint on the DAP
 * interface (pyOCD, OpenOCD, probe-rs) read SWO from it independently of the command pipe.
 *
 * SWO_Task() runs from the main loop (with interrupts masked, see main.c) AND from the endpoint-4 IN-complete interrupt, so a
 * long SWD command in the main loop cannot starve the endpoint while trace data is waiting.
 */
#include <stdint.h>
#include "usb_lib.h"
#include "usb_regs.h"
#include "usb_desc.h"
#include "DAP_config.h"
#include "DAP.h"

#if (SWO_STREAM != 0)

static const uint8_t * volatile tx_ptr;
static volatile uint32_t        tx_left;

static void send_chunk(void)
{
    uint32_t n = tx_left;

    if (n > SWO_BULK_IN_SZ) { n = SWO_BULK_IN_SZ; }
    USB_SIL_Write(SWO_BULK_IN_EP, (uint8_t *)tx_ptr, n);
    tx_ptr  += n;
    tx_left -= n;
    SetEPTxValid(ENDP4);
}

void SWO_QueueTransfer(uint8_t *buf, uint32_t num)
{
    if (num == 0U) {
        SWO_TransferComplete();
        return;
    }
    tx_ptr  = buf;
    tx_left = num;
    send_chunk();
}

void SWO_AbortTransfer(void)
{
    tx_left = 0U;
    SetEPTxStatus(ENDP4, EP_TX_NAK);
}

/* IN token completed on endpoint 4 (USB interrupt) */
void SWO_EP_InComplete(void)
{
    if (tx_left != 0U) {
        send_chunk();
    } else {
        SWO_TransferComplete();
        SWO_Task();               /* chain the next block right here: the main loop may be busy with a long SWD command */
    }
}

#endif
