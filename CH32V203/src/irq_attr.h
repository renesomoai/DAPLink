#ifndef IRQ_ATTR_H
#define IRQ_ATTR_H
/* WCH's modified GCC (MounRiver) accepts interrupt("WCH-Interrupt-fast"), which relies on the
 * hardware prologue/epilogue (HPE, enabled in startup via INTSYSCR=0x3). Upstream/xPack GCC
 * only knows plain `interrupt`; it is correct with HPE too, just a few cycles slower per IRQ.
 * Build with -DSTD_GCC (CMake does it) to select the portable attribute. */
#ifdef STD_GCC
#define WCH_IRQ_FAST __attribute__((interrupt))
#else
#define WCH_IRQ_FAST __attribute__((interrupt("WCH-Interrupt-fast")))
#endif
#endif
