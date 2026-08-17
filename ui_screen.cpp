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

/*
 * The USER button, identified on hardware with btnscan: 20 clean press/release
 * cycles on GPIO 0 and nothing on any other pin.
 *
 * GPIO 0 is the ESP32-S3 strapping pin, so it is only ever read at runtime.
 * Nothing here depends on its level at reset - holding it during power-up still
 * just enters the ROM download mode, as it should.
 */
#define UI_BTN_PIN      0
#define UI_BTN_DEBOUNCE 30
#define UI_BTN_LONG_MS  700

enum UiPage : uint8_t { PAGE_LINK = 0, PAGE_TEST, PAGE_INFO, PAGE_COUNT };

static bool     s_ok       = false;
static uint32_t s_lastDraw = 0;
static bool     s_layout   = false;
static uint8_t  s_page     = PAGE_LINK;

static uint8_t  s_btnStable = HIGH;
static uint8_t  s_btnLast   = HIGH;
static uint32_t s_btnAt     = 0;
static uint32_t s_pressedAt = 0;
static bool     s_longFired = false;

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
  pinMode(UI_BTN_PIN, INPUT_PULLUP);
  dispBacklight(true);
  dispFill(TFT_BLACK);
  dispText(2, 90, "HaLow tester", TFT_CYAN, TFT_BLACK, 1);
  dispText(2, 104, FW_VERSION, TFT_GREY, TFT_BLACK, 1);
  LOGI("UI", "panel active");
}

bool uiPresent(void) { return s_ok; }

static void drawTestPage(void) {
  const ThroughputResult &X = thrResult();
  const RttStats &T = rttStats();
  char buf[32];
  static const char *state[] = { "idle", "arming", "running", "done", "failed" };

  line(ROW_TITLE, "THROUGHPUT", TFT_CYAN, 2);
  dispFillRect(0, ROW_TITLE + 18, TFT_W, 1, TFT_GREY);

  snprintf(buf, sizeof(buf), "%s %s", X.udp ? "UDP" : "TCP",
           X.dir == THR_DIR_TX ? "TX" : "RX");
  line(ROW_RSSILBL, buf, TFT_GREY, 1);
  line(ROW_RSSIBIG, state[X.state <= 4 ? X.state : 0],
       X.state == THR_RUNNING ? TFT_CYAN : TFT_WHITE, 2);

  uint32_t kbps = (X.state == THR_RUNNING && X.curKbps) ? X.curKbps : X.avgKbps;
  snprintf(buf, sizeof(buf), "%lu kbps", (unsigned long)kbps);
  line(ROW_RATE, buf, TFT_GREEN, 2);

  dispFillRect(0, ROW_SEP1, TFT_W, 1, TFT_GREY);
  snprintf(buf, sizeof(buf), "bytes %lu", (unsigned long)X.bytes);
  line(ROW_RTT, buf, TFT_WHITE, 1);
  snprintf(buf, sizeof(buf), "time  %lu.%lus",
           (unsigned long)(X.durationMs / 1000), (unsigned long)((X.durationMs % 1000) / 100));
  line(ROW_LOSS, buf, TFT_WHITE, 1);
  if (X.udpStatsValid)
    snprintf(buf, sizeof(buf), "loss  %u.%02u %%", X.lossPct100 / 100, X.lossPct100 % 100);
  else
    snprintf(buf, sizeof(buf), "loss  --");
  line(ROW_THR, buf, TFT_WHITE, 1);

  dispFillRect(0, ROW_SEP2, TFT_W, 1, TFT_GREY);
  if (T.valid) snprintf(buf, sizeof(buf), "RTT %u.%u ms", T.lastTenthMs/10, T.lastTenthMs%10);
  else         snprintf(buf, sizeof(buf), "RTT --");
  line(ROW_PEER, buf, TFT_WHITE, 1);

  line(ROW_CHAN, "hold USR:", TFT_YELLOW, 1);
  line(ROW_BW,   "run TCP TX 10s", TFT_YELLOW, 1);
  line(ROW_IP,   "tap USR: next", TFT_GREY, 1);
}

static void drawInfoPage(void) {
  const RadioInfo &R = halowRadioInfo();
  char buf[32];

  line(ROW_TITLE, "RADIO", TFT_CYAN, 2);
  dispFillRect(0, ROW_TITLE + 18, TFT_W, 1, TFT_GREY);

  line(ROW_RSSILBL, "MM6108 firmware", TFT_GREY, 1);
  line(ROW_RSSIBIG + 4, R.morseFwVersion[0] ? R.morseFwVersion : "--", TFT_WHITE, 2);

  snprintf(buf, sizeof(buf), "chip %.20s",
           R.chipIdString[0] ? R.chipIdString : "--");
  line(ROW_RATE, buf, TFT_WHITE, 1);

  dispFillRect(0, ROW_SEP1, TFT_W, 1, TFT_GREY);
  snprintf(buf, sizeof(buf), "BCF %u.%u.%u", R.bcfMajor, R.bcfMinor, R.bcfPatch);
  line(ROW_RTT, buf, TFT_WHITE, 1);
  snprintf(buf, sizeof(buf), "SDK %s", R.sdkVersion);
  line(ROW_LOSS, buf, TFT_GREY, 1);
  snprintf(buf, sizeof(buf), "fw  %s", FW_VERSION);
  line(ROW_THR, buf, TFT_GREY, 1);

  dispFillRect(0, ROW_SEP2, TFT_W, 1, TFT_GREY);
  line(ROW_PEER,   "HaLow MAC", TFT_GREY, 1);
  line(ROW_UPTIME, halowOwnMac().c_str(), TFT_WHITE, 1);
  line(ROW_CHAN,   "mgmt SSID", TFT_GREY, 1);
  line(ROW_BW,     cfgMgmtSsid().c_str(), TFT_WHITE, 1);
  line(ROW_IP,     halowLocalIP().toString().c_str(), TFT_GREY, 1);
  line(ROW_HEAP,   forwardModeName(g_cfg.forwardMode), TFT_GREY, 1);
}

/* Short press cycles pages; a long press performs the page's action. */
static void doShortPress(void) {
  s_page = (uint8_t)((s_page + 1) % PAGE_COUNT);
  s_layout = false;
  LOGI("UI", "page -> %u", s_page);
}

static void doLongPress(void) {
  switch (s_page) {
    case PAGE_TEST: {
      String err;
      if (thrStartTest(THR_DIR_TX, false, 10, 0, 0, err)) {
        LOGI("UI", "TCP TX test started from the panel button");
      } else {
        LOGW("UI", "test could not start: %s", err.c_str());
      }
      break;
    }
    default: {
      bool on = !rttProbing();
      rttSetProbing(on, g_cfg.contIntervalMs ? g_cfg.contIntervalMs : 1000);
      LOGI("UI", "continuous probe %s from the panel button", on ? "on" : "off");
      break;
    }
  }
  s_layout = false;
}

static void pollButton(void) {
  uint8_t raw = digitalRead(UI_BTN_PIN);
  uint32_t now = millis();

  if (raw != s_btnLast) { s_btnLast = raw; s_btnAt = now; }
  if ((uint32_t)(now - s_btnAt) < UI_BTN_DEBOUNCE) return;

  if (raw != s_btnStable) {
    s_btnStable = raw;
    if (raw == LOW) {                 /* pressed */
      s_pressedAt = now;
      s_longFired = false;
    } else {                          /* released */
      if (!s_longFired) doShortPress();
    }
  } else if (raw == LOW && !s_longFired &&
             (uint32_t)(now - s_pressedAt) >= UI_BTN_LONG_MS) {
    s_longFired = true;               /* fire once, while still held */
    doLongPress();
  }
}

void uiTick(void) {
  if (!s_ok) return;

  pollButton();   /* every loop, so presses are never missed */

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

  if (s_page == PAGE_TEST) { drawTestPage(); return; }
  if (s_page == PAGE_INFO) { drawInfoPage(); return; }

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
  /*
   * Distinguish "the probe is switched off" from "the probe is running but has
   * no answer yet". A bare dash for both reads as a fault when it is really
   * just a disabled feature.
   */
  if (!rttProbing()) {
    line(ROW_RTT,  "RTT   off", TFT_GREY, 1);
    line(ROW_LOSS, "  hold USR", TFT_YELLOW, 1);
  } else {
    if (T.valid) snprintf(buf, sizeof(buf), "RTT   %u.%u ms",
                          T.lastTenthMs / 10, T.lastTenthMs % 10);
    else         snprintf(buf, sizeof(buf), "RTT   waiting");
    line(ROW_RTT, buf, TFT_WHITE, 1);

    if (T.lossPct100 != SAMPLE_NA_U16)
      snprintf(buf, sizeof(buf), "LOSS  %u.%02u %%", T.lossPct100 / 100, T.lossPct100 % 100);
    else
      snprintf(buf, sizeof(buf), "LOSS  --");
    line(ROW_LOSS, buf,
         (T.lossPct100 != SAMPLE_NA_U16 && T.lossPct100 > 500) ? TFT_RED : TFT_WHITE, 1);
  }

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
