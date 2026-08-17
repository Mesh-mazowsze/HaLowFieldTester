/*
 * stats.h - in-RAM history ring buffers and CSV/JSON export.
 *
 * Two rings so the panel can show both a detailed recent window and a longer
 * trend without keeping an hour of per-second samples:
 *   fast: 1 s resolution, 15 minutes
 *   slow: 6 s resolution, 60 minutes
 * Nothing is persisted to flash.
 */
#pragma once

#include <Arduino.h>

#define SAMPLE_NA_RSSI  ((int16_t)-32768)
#define SAMPLE_NA_MCS   ((int8_t)-1)

#define HIST_FAST_LEN   900   /* 900 x 1 s  = 15 min */
#define HIST_SLOW_LEN   600   /* 600 x 6 s  = 60 min */
#define HIST_SLOW_DIV   6

/* Bit values for Sample.flags */
#define SF_SGI      0x01
#define SF_LINK_UP  0x02
#define SF_PEER_OK  0x04   /* peer telemetry was fresh for this sample */

struct Sample {
  uint32_t tMs;
  int16_t  rssi;        /* dBm, SAMPLE_NA_RSSI if unknown */
  int8_t   mcs;         /* SAMPLE_NA_MCS if unknown */
  uint8_t  bwMhz;       /* 0 if unknown */
  uint8_t  flags;
  uint8_t  _pad;
  uint32_t phyKbps;     /* 0 if unknown */
  uint16_t rttTenthMs;  /* RTT in 0.1 ms units; 0xFFFF if unknown */
  uint16_t lossPct100;  /* RTT probe loss, hundredths of a percent; 0xFFFF unknown */
  uint16_t perPct100;   /* PHY error rate from rate-control stats; 0xFFFF unknown */
  uint32_t thrKbps;     /* most recent throughput measurement, 0 if none */
};

#define SAMPLE_NA_U16 0xFFFF

void  histInit(void);
void  histPush(const Sample &s);
void  histClear(void);

/* window: 0 = fast ring (15 min), 1 = slow ring (60 min) */
uint16_t histCount(uint8_t window);
bool     histGet(uint8_t window, uint16_t index, Sample &out); /* index 0 = oldest */

/*
 * Serialises history as JSON. `tail` limits the output to that many most
 * recent samples (0 = the whole ring); `maxPoints` then decimates evenly so
 * the browser never has to parse - and the device never has to build - more
 * than it can draw.
 */
void histJson(String &out, uint8_t window, uint16_t maxPoints, uint16_t tail = 0);

/* CSV export of the fast ring, one row per sample. */
void histCsvHeader(String &out);
void histCsvRows(String &out, uint8_t window);

/*
 * Range variants used by the HTTP export handlers. A full ring serialises to
 * ~80 KB, which is too large to hold in one String alongside everything else,
 * so the exports are streamed in batches instead.
 */
void histCsvRowsRange(String &out, uint8_t window, uint16_t from, uint16_t count);
void histJsonPointsRange(String &out, uint8_t window, uint16_t from, uint16_t count);
