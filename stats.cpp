#include "stats.h"

static Sample  s_fast[HIST_FAST_LEN];
static Sample  s_slow[HIST_SLOW_LEN];
static uint16_t s_fastHead = 0, s_fastCount = 0;
static uint16_t s_slowHead = 0, s_slowCount = 0;
static uint8_t  s_slowDiv  = 0;

void histInit(void) {
  histClear();
}

void histClear(void) {
  s_fastHead = s_fastCount = 0;
  s_slowHead = s_slowCount = 0;
  s_slowDiv  = 0;
}

void histPush(const Sample &s) {
  s_fast[s_fastHead] = s;
  s_fastHead = (uint16_t)((s_fastHead + 1) % HIST_FAST_LEN);
  if (s_fastCount < HIST_FAST_LEN) s_fastCount++;

  if (++s_slowDiv >= HIST_SLOW_DIV) {
    s_slowDiv = 0;
    s_slow[s_slowHead] = s;
    s_slowHead = (uint16_t)((s_slowHead + 1) % HIST_SLOW_LEN);
    if (s_slowCount < HIST_SLOW_LEN) s_slowCount++;
  }
}

static Sample *ringBuf(uint8_t window)    { return window ? s_slow : s_fast; }
static uint16_t ringLen(uint8_t window)   { return window ? HIST_SLOW_LEN : HIST_FAST_LEN; }
static uint16_t ringHead(uint8_t window)  { return window ? s_slowHead : s_fastHead; }
static uint16_t ringCount(uint8_t window) { return window ? s_slowCount : s_fastCount; }

uint16_t histCount(uint8_t window) {
  return ringCount(window);
}

bool histGet(uint8_t window, uint16_t index, Sample &out) {
  uint16_t count = ringCount(window);
  if (index >= count) return false;
  uint16_t len    = ringLen(window);
  uint16_t oldest = (uint16_t)((ringHead(window) + len - count) % len);
  out = ringBuf(window)[(oldest + index) % len];
  return true;
}

/* Helpers that emit `null` rather than a fabricated value. */
static void jsonInt(String &out, long v)       { out += v; }
static void jsonNull(String &out)              { out += "null"; }

static void jsonRssi(String &out, int16_t v) {
  if (v == SAMPLE_NA_RSSI) jsonNull(out); else jsonInt(out, v);
}
static void jsonMcs(String &out, int8_t v) {
  if (v == SAMPLE_NA_MCS) jsonNull(out); else jsonInt(out, v);
}
static void jsonKbps(String &out, uint32_t v) {
  if (v == 0) jsonNull(out); else jsonInt(out, (long)v);
}
static void jsonPct100(String &out, uint16_t v) {
  if (v == SAMPLE_NA_U16) { jsonNull(out); return; }
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%02u", (unsigned)(v / 100), (unsigned)(v % 100));
  out += buf;
}
static void jsonRtt(String &out, uint16_t v) {
  if (v == SAMPLE_NA_U16) { jsonNull(out); return; }
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u", (unsigned)(v / 10), (unsigned)(v % 10));
  out += buf;
}

/* Emits a single point object (without any leading comma). */
static void emitPoint(String &out, const Sample &s) {
  out += "{\"t\":";
  out += s.tMs;
  out += ",\"rssi\":";   jsonRssi(out, s.rssi);
  out += ",\"mcs\":";    jsonMcs(out, s.mcs);
  out += ",\"phy\":";    jsonKbps(out, s.phyKbps);
  out += ",\"rtt\":";    jsonRtt(out, s.rttTenthMs);
  out += ",\"loss\":";   jsonPct100(out, s.lossPct100);
  out += ",\"per\":";    jsonPct100(out, s.perPct100);
  out += ",\"thr\":";    jsonKbps(out, s.thrKbps);
  out += ",\"up\":";     out += (s.flags & SF_LINK_UP) ? 1 : 0;
  out += '}';
}

void histJsonPointsRange(String &out, uint8_t window, uint16_t from, uint16_t count) {
  Sample s;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t idx = (uint16_t)(from + i);
    if (!histGet(window, idx, s)) break;
    if (idx) out += ',';
    emitPoint(out, s);
  }
}

void histJson(String &out, uint8_t window, uint16_t maxPoints, uint16_t tail) {
  uint16_t total = ringCount(window);
  uint16_t first = 0;
  uint16_t count = total;
  if (tail && tail < total) {
    first = (uint16_t)(total - tail);
    count = tail;
  }
  if (maxPoints == 0) maxPoints = 1;
  uint16_t step = (uint16_t)((count + maxPoints - 1) / maxPoints);
  if (step == 0) step = 1;

  out.reserve(out.length() + (size_t)(count / step) * 96 + 64);
  out += "{\"window\":";
  out += window;
  out += ",\"step_s\":";
  out += (window ? HIST_SLOW_DIV : 1);
  out += ",\"points\":[";

  bool isFirst = true;
  Sample s;
  for (uint16_t i = 0; i < count; i += step) {
    if (!histGet(window, (uint16_t)(first + i), s)) break;
    if (!isFirst) out += ',';
    isFirst = false;
    out += "{\"t\":";
    out += s.tMs;
    out += ",\"rssi\":";   jsonRssi(out, s.rssi);
    out += ",\"mcs\":";    jsonMcs(out, s.mcs);
    out += ",\"phy\":";    jsonKbps(out, s.phyKbps);
    out += ",\"rtt\":";    jsonRtt(out, s.rttTenthMs);
    out += ",\"loss\":";   jsonPct100(out, s.lossPct100);
    out += ",\"per\":";    jsonPct100(out, s.perPct100);
    out += ",\"thr\":";    jsonKbps(out, s.thrKbps);
    out += ",\"up\":";     out += (s.flags & SF_LINK_UP) ? 1 : 0;
    out += '}';
  }
  out += "]}";
}

void histCsvHeader(String &out) {
  /*
   * Columns the MM6108 cannot provide are present but always empty, so the
   * schema stays stable and no value is ever invented:
   *   snr, noise_dbm, tx_power_dbm, tx_packets, rx_packets
   */
  out += "timestamp_ms,rssi_dbm,snr_db,noise_dbm,mcs,bw_mhz,sgi,phy_rate_kbps,"
         "rtt_ms,loss_pct,phy_per_pct,throughput_kbps,link_up,tx_power_dbm,"
         "tx_packets,rx_packets\n";
}

static void csvRtt(String &out, uint16_t v) {
  if (v == SAMPLE_NA_U16) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u", (unsigned)(v / 10), (unsigned)(v % 10));
  out += buf;
}

static void csvPct100(String &out, uint16_t v) {
  if (v == SAMPLE_NA_U16) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%02u", (unsigned)(v / 100), (unsigned)(v % 100));
  out += buf;
}

void histCsvRows(String &out, uint8_t window) {
  histCsvRowsRange(out, window, 0, ringCount(window));
}

void histCsvRowsRange(String &out, uint8_t window, uint16_t from, uint16_t count) {
  Sample s;
  out.reserve(out.length() + (size_t)count * 80);

  for (uint16_t i = 0; i < count; i++) {
    if (!histGet(window, (uint16_t)(from + i), s)) break;
    char head[64];
    snprintf(head, sizeof(head), "%lu,", (unsigned long)s.tMs);
    out += head;

    if (s.rssi != SAMPLE_NA_RSSI) out += s.rssi;
    out += ",";        /* snr        - not available from the MM6108 API */
    out += ",";        /* noise_dbm  - only reported in scan results     */
    if (s.mcs != SAMPLE_NA_MCS) out += s.mcs;
    out += ",";
    if (s.bwMhz) out += s.bwMhz;
    out += ",";
    out += (s.flags & SF_SGI) ? "1" : "0";
    out += ",";
    if (s.phyKbps) out += s.phyKbps;
    out += ",";
    csvRtt(out, s.rttTenthMs);
    out += ",";
    csvPct100(out, s.lossPct100);
    out += ",";
    csvPct100(out, s.perPct100);
    out += ",";
    if (s.thrKbps) out += s.thrKbps;
    out += ",";
    out += (s.flags & SF_LINK_UP) ? "1" : "0";
    /* tx_power_dbm, tx_packets, rx_packets: not readable, left empty. */
    out += ",,,\n";
  }
}
