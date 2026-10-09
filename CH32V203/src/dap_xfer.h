/*
 * dap_xfer: USB-independent transport for CMSIS-DAP v2 over a Full Speed bulk pair (64 B packets) when
 * DAP_PACKET_SIZE is larger than the endpoint size.
 *
 *  RX: USB OUT packets are reassembled into whole DAP requests. A request ends at
 *        - a short packet or a zero-length packet (pyOCD sends a ZLP after multiples of 64 smaller than the
 *          packet size), or
 *        - the request length derived from the command itself (a request of exactly DAP_PACKET_SIZE bytes is
 *          not followed by a ZLP).
 *      DAP_TransferAbort (1-byte packet at a request boundary) is acted on immediately, outside the queue.
 *  TX: responses larger than 64 B are sent as consecutive 64 B packets. A response whose length is a multiple
 *      of 64 and smaller than DAP_PACKET_SIZE is closed with a ZLP (the host reads DAP_PACKET_SIZE bytes).
 *  Validation: a request whose response could not fit in DAP_PACKET_SIZE is answered with {id, DAP_ERROR}
 *      without being executed (previously a host exceeding the advertised packet size overflowed the buffers).
 *
 * No hardware access here; usb_istr.c is the thin glue. Unit-tested in the virtual bench (sim/).
 */
#ifndef DAP_XFER_H
#define DAP_XFER_H

#include <stdint.h>

#define DAP_XFER_MPS        64U        /* USB FS bulk max packet size */
#define DAP_LEN_UNKNOWN     0xFFFFU

typedef struct {
    uint32_t ring_full;      /* request dropped: no free request slot */
    uint32_t oversize;       /* request dropped: longer than DAP_PACKET_SIZE */
    uint32_t rejected;       /* request refused by validation (response would not fit) */
    uint32_t len_mismatch;   /* diagnostic: length from dap_request_length() != length consumed by DAP.c */
} dap_xfer_stats_t;

extern dap_xfer_stats_t dap_xfer_stats;

/* Total request length (including the command byte) deduced from the first `avail` bytes.
 * Returns 0 if more bytes are needed to decide, DAP_LEN_UNKNOWN for commands it does not know (vendor...). */
uint32_t dap_request_length(const uint8_t *req, uint32_t avail);

/* Upper bound of the response size for a complete request (bytes), computed before executing it. */
uint32_t dap_response_bound(const uint8_t *req, uint32_t len);

/* USB OUT packet received (call from the USB interrupt). n may be 0 (ZLP). */
void dap_rx_feed(const uint8_t *pkt, uint32_t n);

/* Execute one queued request and queue its response. Returns 1 if a request was processed. Main loop. */
uint32_t dap_xfer_process(void);

/* Next USB IN packet: returns its length (0 = ZLP) and sets *ptr, or -1 if nothing is pending.
 * Call when idle (with interrupts masked) and from every IN-complete event. */
int32_t dap_tx_next(const uint8_t **ptr);

#endif
