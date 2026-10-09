#include <string.h>
#include "DAP_config.h"
#include "DAP.h"
#include "dap_xfer.h"

#define NSLOT   DAP_PACKET_COUNT
#define SSIZE   DAP_PACKET_SIZE
#define MPS     DAP_XFER_MPS

#if (NSLOT & (NSLOT - 1U)) != 0U
#error "DAP_PACKET_COUNT must be a power of two (free-running ring indices)"
#endif

#ifndef SWD_SEQUENCE_CLK
#define SWD_SEQUENCE_CLK    0x3FU
#endif
#ifndef SWD_SEQUENCE_DIN
#define SWD_SEQUENCE_DIN    0x80U
#endif
#ifndef JTAG_SEQUENCE_TCK
#define JTAG_SEQUENCE_TCK   0x3FU
#endif
#ifndef JTAG_SEQUENCE_TDO
#define JTAG_SEQUENCE_TDO   0x80U
#endif

dap_xfer_stats_t dap_xfer_stats;

static uint8_t  req_buf[NSLOT][SSIZE];
static uint16_t req_len[NSLOT];
static uint8_t  resp_buf[NSLOT][SSIZE];
static uint16_t resp_len[NSLOT];

/* Single producer / single consumer rings with free-running counters (no shared read-modify-write). */
static volatile uint32_t req_in;     /* written by the USB interrupt only */
static volatile uint32_t req_out;    /* written by the main loop only */
static volatile uint32_t resp_in;    /* written by the main loop only */
static volatile uint32_t resp_out;   /* written by dap_tx_next() (IN-complete interrupt or main with IRQs masked) */

static uint32_t rx_fill;             /* bytes already stored in the request being assembled */
static uint8_t  rx_drop;             /* discarding the current request (no slot / too long) until it ends */

enum { TX_IDLE = 0, TX_SENDING, TX_ZLP, TX_LAST };
static uint8_t  tx_state;
static uint32_t tx_off;

static uint32_t seq_bytes(uint32_t cycles) {
    return (cycles + 7U) >> 3;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Request length                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------ */
uint32_t dap_request_length(const uint8_t *r, uint32_t avail) {
    uint32_t pos, n, i, info;

    if (avail == 0U) { return 0U; }

    switch (r[0]) {
        case ID_DAP_Info:             return 2U;
        case ID_DAP_HostStatus:       return 3U;
        case ID_DAP_Connect:          return 2U;
        case ID_DAP_Disconnect:       return 1U;
        case ID_DAP_TransferConfigure:return 6U;
        case ID_DAP_TransferAbort:    return 1U;
        case ID_DAP_WriteABORT:       return 6U;
        case ID_DAP_Delay:            return 3U;
        case ID_DAP_ResetTarget:      return 1U;
        case ID_DAP_SWJ_Pins:         return 7U;
        case ID_DAP_SWJ_Clock:        return 5U;
        case ID_DAP_SWD_Configure:    return 2U;
        case ID_DAP_JTAG_IDCODE:      return 2U;
        case ID_DAP_SWO_Transport:    return 2U;
        case ID_DAP_SWO_Mode:         return 2U;
        case ID_DAP_SWO_Baudrate:     return 5U;
        case ID_DAP_SWO_Control:      return 2U;
        case ID_DAP_SWO_Status:       return 1U;
        case ID_DAP_SWO_ExtendedStatus: return 2U;
        case ID_DAP_SWO_Data:         return 3U;

        case ID_DAP_SWJ_Sequence:
            if (avail < 2U) { return 0U; }
            n = (r[1] == 0U) ? 256U : r[1];
            return 2U + seq_bytes(n);

        case ID_DAP_JTAG_Configure:
            if (avail < 2U) { return 0U; }
            return 2U + r[1];

        case ID_DAP_Transfer:
            if (avail < 3U) { return 0U; }
            n = r[2];
            pos = 3U;
            for (i = 0U; i < n; i++) {
                if (pos >= avail) { return 0U; }
                info = r[pos++];
                if ((info & DAP_TRANSFER_RnW) != 0U) {
                    if ((info & DAP_TRANSFER_MATCH_VALUE) != 0U) { pos += 4U; }
                } else {
                    pos += 4U;                       /* write data, or match mask */
                }
            }
            return pos;

        case ID_DAP_TransferBlock:
            if (avail < 5U) { return 0U; }
            n = (uint32_t)r[2] | ((uint32_t)r[3] << 8);
            return ((r[4] & DAP_TRANSFER_RnW) != 0U) ? 5U : (5U + 4U * n);

        case ID_DAP_SWD_Sequence:
            if (avail < 2U) { return 0U; }
            n = r[1];
            pos = 2U;
            for (i = 0U; i < n; i++) {
                if (pos >= avail) { return 0U; }
                info = r[pos++];
                if ((info & SWD_SEQUENCE_DIN) == 0U) {
                    uint32_t c = info & SWD_SEQUENCE_CLK;
                    pos += seq_bytes((c == 0U) ? 64U : c);
                }
            }
            return pos;

        case ID_DAP_JTAG_Sequence:
            if (avail < 2U) { return 0U; }
            n = r[1];
            pos = 2U;
            for (i = 0U; i < n; i++) {
                if (pos >= avail) { return 0U; }
                info = r[pos++];
                {
                    uint32_t c = info & JTAG_SEQUENCE_TCK;
                    pos += seq_bytes((c == 0U) ? 64U : c);
                }
            }
            return pos;

        default:
            return DAP_LEN_UNKNOWN;
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Response bound (so that no request can make DAP.c write past DAP_PACKET_SIZE)                                   */
/* ------------------------------------------------------------------------------------------------------------ */
uint32_t dap_response_bound(const uint8_t *r, uint32_t len) {
    uint32_t pos, n, i, info, bound;

    switch (r[0]) {
        case ID_DAP_Transfer:
            if (len < 3U) { return 0xFFFFU; }
            n = r[2];
            pos = 3U;
            bound = 3U + 4U;                                  /* header + final posted read */
            for (i = 0U; i < n; i++) {
                if (pos >= len) { return 0xFFFFU; }
                info = r[pos++];
                if ((info & DAP_TRANSFER_RnW) != 0U) {
                    bound += 4U;
                    if ((info & DAP_TRANSFER_MATCH_VALUE) != 0U) { pos += 4U; }
                } else {
                    pos += 4U;
                }
                if ((info & DAP_TRANSFER_TIMESTAMP) != 0U) { bound += 4U; }
            }
            return bound;

        case ID_DAP_TransferBlock:
            if (len < 5U) { return 0xFFFFU; }
            n = (uint32_t)r[2] | ((uint32_t)r[3] << 8);
            return ((r[4] & DAP_TRANSFER_RnW) != 0U) ? (4U + 4U * n) : 4U;

        case ID_DAP_SWD_Sequence:
            if (len < 2U) { return 0xFFFFU; }
            n = r[1];
            pos = 2U;
            bound = 2U;
            for (i = 0U; i < n; i++) {
                if (pos >= len) { return 0xFFFFU; }
                info = r[pos++];
                {
                    uint32_t c = info & SWD_SEQUENCE_CLK;
                    uint32_t b = seq_bytes((c == 0U) ? 64U : c);
                    if ((info & SWD_SEQUENCE_DIN) != 0U) { bound += b; } else { pos += b; }
                }
            }
            return bound;

        case ID_DAP_JTAG_Sequence:
            if (len < 2U) { return 0xFFFFU; }
            n = r[1];
            pos = 2U;
            bound = 2U;
            for (i = 0U; i < n; i++) {
                if (pos >= len) { return 0xFFFFU; }
                info = r[pos++];
                {
                    uint32_t c = info & JTAG_SEQUENCE_TCK;
                    uint32_t b = seq_bytes((c == 0U) ? 64U : c);
                    pos += b;
                    if ((info & JTAG_SEQUENCE_TDO) != 0U) { bound += b; }
                }
            }
            return bound;

        default:
            return 1U;      /* all other commands answer with a small fixed size (or bound it themselves, e.g. SWO_Data) */
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* RX: reassemble requests                                                                                         */
/* ------------------------------------------------------------------------------------------------------------ */
static void rx_complete(void) {
    req_len[req_in & (NSLOT - 1U)] = (uint16_t)rx_fill;
    rx_fill = 0U;
    req_in = req_in + 1U;
}

void dap_rx_feed(const uint8_t *p, uint32_t n) {
    uint8_t *slot;
    uint32_t need;

    if ((rx_fill == 0U) && (rx_drop == 0U) && (n == 1U) && (p[0] == ID_DAP_TransferAbort)) {
        DAP_TransferAbort = 1U;                          /* act at once; never queued */
        return;
    }

    if (n == 0U) {                                       /* ZLP: closes the request in progress */
        if (rx_drop != 0U) {
            rx_drop = 0U;
        } else if (rx_fill != 0U) {
            rx_complete();
        }
        return;
    }

    if (rx_drop == 0U) {
        if ((rx_fill == 0U) && ((req_in - req_out) >= NSLOT)) {
            dap_xfer_stats.ring_full++;
            rx_drop = 1U;
        } else if ((rx_fill + n) > SSIZE) {
            dap_xfer_stats.oversize++;
            rx_fill = 0U;
            rx_drop = 1U;
        }
    }
    if (rx_drop != 0U) {
        if (n < MPS) { rx_drop = 0U; }                   /* a short packet ends the discarded request */
        return;
    }

    slot = req_buf[req_in & (NSLOT - 1U)];
    memcpy(&slot[rx_fill], p, n);
    rx_fill += n;

    need = dap_request_length(slot, rx_fill);
    if ((need != 0U) && (need != DAP_LEN_UNKNOWN) && (need > SSIZE)) {
        dap_xfer_stats.oversize++;                       /* announced longer than the advertised packet size */
        rx_fill = 0U;
        rx_drop = (n < MPS) ? 0U : 1U;
        return;
    }
    if ((need != 0U) && (need != DAP_LEN_UNKNOWN) && (rx_fill >= need)) {
        rx_complete();                                   /* complete by its own length (no ZLP needed) */
    } else if (n < MPS) {
        rx_complete();                                   /* short packet terminates (unknown or malformed too) */
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Process and TX                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------ */
uint32_t dap_xfer_process(void) {
    const uint8_t *rq;
    uint8_t *rs;
    uint32_t len, bound, rl, r, ri, wi;

    if (req_in == req_out) { return 0U; }
    if ((resp_in - resp_out) >= NSLOT) { return 0U; }    /* response ring full: wait for the IN endpoint to drain */

    ri = req_out & (NSLOT - 1U);
    wi = resp_in & (NSLOT - 1U);
    rq = req_buf[ri];
    rs = resp_buf[wi];
    len = req_len[ri];

    bound = dap_response_bound(rq, len);
    if (bound > SSIZE) {
        rs[0] = rq[0];
        rs[1] = DAP_ERROR;
        rl = 2U;
        dap_xfer_stats.rejected++;
    } else {
        r = DAP_ProcessCommand(rq, rs);
        rl = r & 0xFFFFU;
        /* Commands the build does not implement (e.g. SWO with SWO_UART=0) are answered with ID_DAP_Invalid and
         * consume one byte in DAP.c; only count real disagreements. */
        if (((r >> 16) != len) && (rs[0] != ID_DAP_Invalid)) { dap_xfer_stats.len_mismatch++; }
    }
    resp_len[wi] = (uint16_t)rl;
    resp_in = resp_in + 1U;
    req_out = req_out + 1U;
    return 1U;
}

int32_t dap_tx_next(const uint8_t **ptr) {
    uint32_t slot, len, chunk;

    if (tx_state == TX_LAST) {                           /* previous response completely delivered */
        resp_out = resp_out + 1U;
        tx_state = TX_IDLE;
    }
    if (tx_state == TX_IDLE) {
        if (resp_in == resp_out) { return -1; }
        tx_off = 0U;
        tx_state = TX_SENDING;
    }

    slot = resp_out & (NSLOT - 1U);
    len = resp_len[slot];

    if ((tx_state == TX_SENDING) && (tx_off < len)) {
        chunk = len - tx_off;
        if (chunk > MPS) { chunk = MPS; }
        *ptr = &resp_buf[slot][tx_off];
        tx_off += chunk;
        if (tx_off >= len) {
            tx_state = (((len % MPS) == 0U) && (len < SSIZE)) ? TX_ZLP : TX_LAST;
        }
        return (int32_t)chunk;
    }

    /* zero-length packet: closes a response that is a multiple of MPS and shorter than DAP_PACKET_SIZE */
    *ptr = resp_buf[slot];
    tx_state = TX_LAST;
    return 0;
}
