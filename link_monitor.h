/*
 * link_monitor.h - periodic sampling of everything the MM6108 will actually tell us.
 *
 * Sources, all real:
 *   RSSI          mmwlan_get_rssi()                (STA role only - see note)
 *   MCS/BW/GI     mmwlan_get_rc_stats()            (delta of the rate table)
 *   PHY error rate mmwlan_get_rc_stats()           (success vs attempts)
 *   UMAC counters mmwlan_get_umac_stats()
 *   Duty cycle    mmwlan_get_duty_cycle_stats()
 *
 * Note on RSSI: mmwlan_get_rssi() measures the signal received *from the AP*,
 * so it is only meaningful on the STA node. The AP node shows the STA's RSSI
 * as reported over the peer telemetry link, clearly labelled as such.
 */
#pragma once

#include <Arduino.h>

struct LinkStats {
  /* --- signal --- */
  bool     rssiValid;
  int16_t  rssiDbm;

  /* --- modulation, from the rate control table --- */
  bool     rateValid;
  int8_t   mcs;
  uint8_t  bwMhz;
  bool     shortGi;
  uint32_t phyRateKbps;     /* looked up from MCS/BW/GI, 0 if undefined */
  uint32_t rateAgeMs;       /* how long since the rate table last moved */

  /* --- PHY-level error rate over the last interval --- */
  bool     perValid;
  uint16_t perPct100;       /* hundredths of a percent */
  uint32_t framesAttempted; /* cumulative, from the rate table */
  uint32_t framesSucceeded;

  /* --- UMAC counters (cumulative) --- */
  bool     umacValid;
  int16_t  umacRssi;
  uint32_t txqDropped;
  uint32_t rxqDropped;
  uint32_t rxCcmpFailures;
  uint32_t rxAllocFailures;
  uint32_t rxReorderTimedout;
  uint16_t hwRestarts;

  /* --- duty cycle (EU 868 is duty-cycle limited) --- */
  bool     dutyValid;
  uint32_t dutyCyclePct100;
  uint8_t  dutyMode;

  /* --- link --- */
  bool     linkUp;
  uint32_t linkUptimeMs;
  uint32_t disconnects;
};

void             linkMonitorInit(void);
void             linkMonitorTick(void);        /* call often; samples once a second */
const LinkStats &linkStats(void);
void             linkMonitorReset(void);       /* clears counters and history */
