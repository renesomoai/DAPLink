#ifndef SWO_EP_CONFIG_H
#define SWO_EP_CONFIG_H

/* SWO streaming endpoint (CMSIS-DAP v2 transport 2, bulk IN 0x84): on by default for the v2 (WinUSB) build.
 * Build with -DONBOARDDAP_NO_SWO_ENDPOINT (CMake option ONBOARDDAP_SWO_ENDPOINT=OFF) to fall back to DAP_SWO_Data polling:
 * pyOCD then reads SWO 508 bytes per command (~380 kB/s) instead of 64 bytes per USB read (~48 kB/s), at the price of
 * taking the command pipe for every poll (no concurrent SWD from another thread). */
#if defined(DAP_FW_V1) || defined(ONBOARDDAP_NO_SWO_ENDPOINT)
#define SWO_EP_ENABLED 0
#else
#define SWO_EP_ENABLED 1
#endif

#endif
