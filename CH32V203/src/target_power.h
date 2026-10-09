#ifndef TARGET_POWER_H
#define TARGET_POWER_H
#include <stdint.h>

/* VTREF sense: header VTREF rail -> 100k/100k divider -> PB1 (ADC1_IN9). Returns the VTREF voltage in mV
 * (average of 8 conversions, VDDA taken as 3300 mV), or 0xFFFF if the ADC did not finish (timeout).
 * NOTE: on T1 the VTREF pin is fed through an ideal diode from the probe's own 3V3 whenever USB is present, so this
 * reads the rail voltage at the header (probe or target supplied), not "target has its own supply". */
uint16_t target_vtref_mv(void);

#define VTREF_MV_PRESENT_MIN  1700U     /* nRF54L15 minimum VDD */
#define VTREF_MV_ERROR        0xFFFFU

#endif
