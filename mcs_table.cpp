#include "mcs_table.h"

/*
 * Rates in kbps, 1 spatial stream, [LGI, SGI].
 * Index 0..9 = MCS0..MCS9. MCS10 (1 MHz repetition mode) is handled separately.
 * A zero entry means "not defined at this bandwidth".
 */
static const uint16_t k1MHz[10][2] = {
  {  300,  333 }, {  600,  667 }, {  900, 1000 }, { 1200, 1333 }, { 1800, 2000 },
  { 2400, 2667 }, { 2700, 3000 }, { 3000, 3333 }, { 3600, 4000 }, {    0,    0 },
};

static const uint16_t k2MHz[10][2] = {
  {  650,  722 }, { 1300, 1444 }, { 1950, 2167 }, { 2600, 2889 }, { 3900, 4333 },
  { 5200, 5778 }, { 5850, 6500 }, { 6500, 7222 }, { 7800, 8667 }, { 8667, 9630 },
};

static const uint16_t k4MHz[10][2] = {
  {  1350,  1500 }, {  2700,  3000 }, {  4050,  4500 }, {  5400,  6000 },
  {  8100,  9000 }, { 10800, 12000 }, { 12150, 13500 }, { 13500, 15000 },
  { 16200, 18000 }, { 18000, 20000 },
};

static const uint16_t k8MHz[10][2] = {
  {  2925,  3250 }, {  5850,  6500 }, {  8775,  9750 }, { 11700, 13000 },
  { 17550, 19500 }, { 23400, 26000 }, { 26325, 29250 }, { 29250, 32500 },
  { 35100, 39000 }, { 39000, 43333 },
};

/* MCS10 exists only at 1 MHz: MCS0 with 2x repetition, so half the rate. */
static const uint16_t kMcs10_1MHz[2] = { 150, 167 };

uint8_t rcBwFieldToMhz(uint8_t bwField) {
  switch (bwField) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    case 3: return 8;
    default: return 0;
  }
}

uint32_t phyRateKbps(uint8_t mcs, uint8_t bwMhz, bool shortGi) {
  const uint8_t gi = shortGi ? 1 : 0;

  if (mcs == 10) {
    /* Only defined for 1 MHz. */
    return (bwMhz == 1) ? kMcs10_1MHz[gi] : 0;
  }
  if (mcs > 9) {
    return 0;
  }

  const uint16_t (*tbl)[2];
  switch (bwMhz) {
    case 1: tbl = k1MHz; break;
    case 2: tbl = k2MHz; break;
    case 4: tbl = k4MHz; break;
    case 8: tbl = k8MHz; break;
    default: return 0;
  }
  return tbl[mcs][gi];
}
