#include "ui_screen.h"
#include "display.h"
#include "config.h"
#include "logbuf.h"
#include "halow_manager.h"
#include "link_monitor.h"
#include "rtt_test.h"
#include "peer_link.h"
#include "throughput_test.h"
#include "stats.h"

#define UI_PERIOD_MS 1000

static bool     s_ok       = false;
static uint32_t s_lastDraw = 0;
static bool     s_layout   = false;

/* Row positions, chosen for the 128x220 portrait panel. */
#define ROW_TITLE   2
#define ROW_RSSILBL 22
#define ROW_RSSIBIG 34
#define ROW_RATE    64
#define ROW_SEP1    80
#define ROW_RTT     86
#define ROW_LOSS    98
#define ROW_THR     110
#define ROW_SEP2    126
#define ROW_PEER    132
#define ROW_UPTIME  144
#define ROW_CHAN    156
#define ROW_BW      168
#define ROW_IP      184
#define ROW_HEAP    196

/* Clears one text line before writing, so leftovers cannot show through. */
static void line(int16_t y, const char *s, uint16_t fg, uint8_t scale) {
  dispFillRect(0, y, TFT_W, (int16_t)(8 * scale), TFT_BLACK);
  dispText(2, y, s, fg, TFT_BLACK, scale);
}

static uint16_t rssiColour(int16_t dbm) {
  if (dbm >= -70) return TFT_GREEN;
  if (dbm >= -90) return TFT_YELLOW;
  return TFT_RED;
}

void uiInit(void) {
  s_ok = dispInit();
  if (!s_ok) {
    LOGI("UI", "no panel fitted - running headless");
    return;
  }
  dispBacklight(true);
  dispFill(TFT_BLACK);
  dispText(2, 90, "HaLow tester", TFT_CYAN, TFT_BLACK, 1);
  dispText(2, 104, FW_VERSION, TFT_GREY, TFT_BLACK, 1);
  LOGI("UI", "panel active");
}

bool uiPresent(void) { return s_ok; }

void uiTick(void) {
  if (!s_ok) return;
  uint32_t now = millis();
  if ((uint32_t)(now - s_lastDraw) < UI_PERIOD_MS) return;
  s_lastDraw = now;

  const LinkStats &L = linkStats();
  const PeerInfo  &P = peerData();
  const RttStats  &T = rttStats();
  const ThroughputResult &X = thrResult();

  char buf[32];

  if (!s_layout) {
    dispFill(TFT_BLACK);
    s_layout = true;
  }

  /* ---- title: role and link state ---- */
  snprintf(buf, sizeof(buf), "%-4s %s", roleName(g_cfg.role),
           L.linkUp ? "LINK UP" : "LINK DOWN");
  line(ROW_TITLE, buf, L.linkUp ? TFT_GREEN : TFT_RED, 2);
  dispFillRect(0, ROW_TITLE + 18, TFT_W, 1, TFT_GREY);

  /*
   * RSSI is measured by the station. On the AP node it arrives over the peer
   * telemetry link, and is marked so the reading is never mistaken for local.
   */
  bool haveRssi = false; int16_t rssi = 0; bool fromPeer = false;
  if (L.rssiValid)                        { rssi = L.rssiDbm;   haveRssi = true; }
  else if (peerIsFresh() && P.rssiValid)  { rssi = P.rssiDbm;   haveRssi = true; fromPeer = true; }

  line(ROW_RSSILBL, fromPeer ? "RSSI (peer)" : "RSSI", TFT_GREY, 1);
  if (haveRssi) {
    snprintf(buf, sizeof(buf), "%d", rssi);
    line(ROW_RSSIBIG, buf, rssiColour(rssi), 3);
    dispText(2 + (int16_t)(strlen(buf) * 18) + 4, ROW_RSSIBIG + 12, "dBm",
             TFT_GREY, TFT_BLACK, 1);
  } else {
    line(ROW_RSSIBIG, "--", TFT_GREY, 3);
  }

  /* ---- modulation ---- */
  int8_t mcs = -1; uint32_t phy = 0;
  if (L.rateValid)                        { mcs = L.mcs; phy = L.phyRateKbps; }
  else if (peerIsFresh() && P.rateValid)  { mcs = P.mcs; phy = P.phyRateKbps; }

  if (mcs >= 0) {
    snprintf(buf, sizeof(buf), "MCS%-2d  %lu.%02lu Mbps", mcs,
             (unsigned long)(phy / 1000), (unsigned long)((phy % 1000) / 10));
  } else {
    snprintf(buf, sizeof(buf), "MCS --  no traffic");
  }
  line(ROW_RATE, buf, TFT_WHITE, 1);
  dispFillRect(0, ROW_SEP1, TFT_W, 1, TFT_GREY);

  /* ---- latency, loss, throughput ---- */
  if (T.valid) snprintf(buf, sizeof(buf), "RTT   %u.%u ms",
                        T.lastTenthMs / 10, T.lastTenthMs % 10);
  else         snprintf(buf, sizeof(buf), "RTT   --");
  line(ROW_RTT, buf, TFT_WHITE, 1);

  if (T.lossPct100 != SAMPLE_NA_U16)
    snprintf(buf, sizeof(buf), "LOSS  %u.%02u %%", T.lossPct100 / 100, T.lossPct100 % 100);
  else
    snprintf(buf, sizeof(buf), "LOSS  --");
  line(ROW_LOSS, buf,
       (T.lossPct100 != SAMPLE_NA_U16 && T.lossPct100 > 500) ? TFT_RED : TFT_WHITE, 1);

  uint32_t kbps = (X.state == THR_RUNNING && X.curKbps) ? X.curKbps : X.avgKbps;
  if (kbps) snprintf(buf, sizeof(buf), "THR   %lu kbps", (unsigned long)kbps);
  else      snprintf(buf, sizeof(buf), "THR   --");
  line(ROW_THR, buf, X.state == THR_RUNNING ? TFT_CYAN : TFT_WHITE, 1);
  dispFillRect(0, ROW_SEP2, TFT_W, 1, TFT_GREY);

  /* ---- peer and link context ---- */
  if (P.valid) {
    snprintf(buf, sizeof(buf), "PEER  %s", peerIsFresh() ? "live" : "stale");
  } else {
    snprintf(buf, sizeof(buf), "PEER  none");
  }
  line(ROW_PEER, buf, (P.valid && peerIsFresh()) ? TFT_GREEN : TFT_GREY, 1);

  uint32_t up = L.linkUptimeMs / 1000;
  snprintf(buf, sizeof(buf), "UP    %02lu:%02lu:%02lu",
           (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60),
           (unsigned long)(up % 60));
  line(ROW_UPTIME, buf, TFT_WHITE, 1);

  ChannelInfo ci;
  if (halowCurrentChannel(ci)) {
    snprintf(buf, sizeof(buf), "CH %-3u %lu.%lu MHz", ci.chanNum,
             (unsigned long)(ci.centreFreqHz / 1000000UL),
             (unsigned long)((ci.centreFreqHz % 1000000UL) / 100000UL));
    line(ROW_CHAN, buf, TFT_WHITE, 1);
    snprintf(buf, sizeof(buf), "%s  BW %u MHz", g_cfg.region, ci.bwMhz);
    line(ROW_BW, buf, TFT_GREY, 1);
  }

  line(ROW_IP, halowLocalIP().toString().c_str(), TFT_GREY, 1);

  snprintf(buf, sizeof(buf), "heap %luk", (unsigned long)(ESP.getFreeHeap() / 1024));
  line(ROW_HEAP, buf, TFT_GREY, 1);
}
