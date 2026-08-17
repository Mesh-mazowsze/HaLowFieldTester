/*
 * mcs_table.h - IEEE 802.11ah (S1G) PHY data rates, 1 spatial stream.
 *
 * The MM6108 driver reports which rate table entry it is transmitting on
 * (MCS index + bandwidth + guard interval) via mmwlan_get_rc_stats(). It does
 * NOT report a data rate in bits/s. This table converts the *measured* MCS /
 * BW / GI triple into the corresponding standard PHY rate.
 *
 * This is a lookup of standardised values from a measured modulation, not an
 * estimate derived from RSSI.
 */
#pragma once

#include <Arduino.h>

/* Bit layout of mmwlan_rc_stats.rate_info (see mmwlan.h):
 *   31         9       8      4    0
 *   +----------+-------+------+----+
 *   | Reserved | Guard | Rate | BW |
 *   +----------+-------+------+----+
 * BW: 0 = 1 MHz, 1 = 2 MHz, 2 = 4 MHz, 3 = 8 MHz
 * Guard: 0 = long GI, 1 = short GI
 *
 * NOTE: this Guard encoding is the inverse of enum mmwlan_gi (where
 * MMWLAN_GI_SHORT == 0). We follow the rate_info documentation here.
 */
#define RC_RATE_INFO_BW(ri)    (uint8_t)(((ri) >> 0) & 0x0F)
#define RC_RATE_INFO_MCS(ri)   (uint8_t)(((ri) >> 4) & 0x0F)
#define RC_RATE_INFO_SGI(ri)   (uint8_t)(((ri) >> 8) & 0x01)

/* Bandwidth field value (0..3) -> MHz. Returns 0 if unknown. */
uint8_t  rcBwFieldToMhz(uint8_t bwField);

/*
 * PHY rate in kbps for a given MCS / bandwidth / guard interval.
 * Returns 0 when the combination is not defined by the standard
 * (e.g. MCS9 at 1 MHz), so callers can report "unknown" instead of guessing.
 */
uint32_t phyRateKbps(uint8_t mcs, uint8_t bwMhz, bool shortGi);
