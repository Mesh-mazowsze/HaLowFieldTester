/*
 * rtt_test.h - round-trip time and packet loss over the HaLow link.
 *
 * Implemented as a small UDP echo protocol rather than ICMP: both nodes run
 * the same firmware, so we control both ends and get an unambiguous
 * timestamped measurement. ICMP on lwIP would need a raw socket bound to the
 * HaLow netif and gives us no control over the responder's turnaround.
 *
 * The prober also serves as the "continuous link test": one small packet per
 * interval, enough to keep the rate table moving and to track the link while
 * driving/walking, without generating iperf-level traffic.
 */
#pragma once

#include <Arduino.h>
#include "stats.h"

struct RttStats {
  bool     valid;
  uint16_t lastTenthMs;
  uint16_t avgTenthMs;
  uint16_t minTenthMs;
  uint16_t maxTenthMs;
  uint32_t sent;
  uint32_t received;
  uint32_t lost;
  uint16_t lossPct100;   /* hundredths of a percent */
};

void  rttInit(void);
void  rttTick(void);

/* The echo responder is always on; the prober is what this enables. */
void  rttSetProbing(bool enable, uint16_t intervalMs);
bool  rttProbing(void);
void  rttResetStats(void);

const RttStats &rttStats(void);

/* Accessors used by the link monitor when building a history sample. */
uint16_t rttLastTenthMs(void);   /* SAMPLE_NA_U16 when unknown */
uint16_t rttLossPct100(void);    /* SAMPLE_NA_U16 when unknown */
