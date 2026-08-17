#include "web_server.h"
#include "web_assets.h"
#include "config.h"
#include "logbuf.h"
#include "halow_manager.h"
#include "link_monitor.h"
#include "rtt_test.h"
#include "throughput_test.h"
#include "peer_link.h"
#include "stats.h"

#include <WiFi.h>
#include <WebServer.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <dhcpserver/dhcpserver.h>
#include <esp_system.h>

#define SSE_PORT        81
/*
 * lwIP is built with CONFIG_LWIP_MAX_SOCKETS = 10 on this platform. Budget:
 * HTTP listen + HTTP client + SSE listen + SSE clients + RTT UDP + peer UDP,
 * leaving headroom for the iperf sessions. Two SSE clients is plenty for a
 * field tester driven from one phone.
 */
#define SSE_MAX_CLIENTS 2
#define SSE_PERIOD_MS   1000

static WebServer  s_http(80);
static WiFiServer s_sse(SSE_PORT);
static WiFiClient s_sseClients[SSE_MAX_CLIENTS];
static uint32_t   s_lastSse = 0;
static uint32_t   s_rebootAt = 0;

/* ------------------------------------------------------------------ */
/* JSON helpers - unavailable values are emitted as null, never faked  */
/* ------------------------------------------------------------------ */

static void jsKey(String &o, const char *k) {
  if (o.length() && o[o.length() - 1] != '{' && o[o.length() - 1] != '[') o += ',';
  o += '"'; o += k; o += "\":";
}
static void jsStr(String &o, const char *k, const char *v) {
  jsKey(o, k);
  if (!v || !*v) { o += "null"; return; }
  o += '"';
  for (const char *p = v; *p; p++) {
    if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
    else if ((uint8_t)*p < 0x20)  { o += ' '; }
    else o += *p;
  }
  o += '"';
}
static void jsStr(String &o, const char *k, const String &v) { jsStr(o, k, v.c_str()); }
static void jsNum(String &o, const char *k, long v)          { jsKey(o, k); o += v; }
static void jsU64(String &o, const char *k, uint64_t v)      { jsKey(o, k); char b[24]; snprintf(b, sizeof(b), "%llu", (unsigned long long)v); o += b; }
static void jsBool(String &o, const char *k, bool v)         { jsKey(o, k); o += v ? "true" : "false"; }
static void jsNull(String &o, const char *k)                 { jsKey(o, k); o += "null"; }
static void jsFix2(String &o, const char *k, uint32_t pct100) {
  jsKey(o, k);
  char b[24];
  snprintf(b, sizeof(b), "%lu.%02lu", (unsigned long)(pct100 / 100), (unsigned long)(pct100 % 100));
  o += b;
}
static void jsFix1(String &o, const char *k, uint32_t tenths) {
  jsKey(o, k);
  char b[24];
  snprintf(b, sizeof(b), "%lu.%lu", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
  o += b;
}

static const char *resetReasonStr(void) {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "unknown";
  }
}

/* ------------------------------------------------------------------ */
/* Status document                                                     */
/* ------------------------------------------------------------------ */

void buildStatusJson(String &out) {
  const LinkStats &L = linkStats();
  const RadioInfo &R = halowRadioInfo();
  const PeerInfo  &P = peerData();
  const RttStats  &T = rttStats();
  const ThroughputResult &X = thrResult();

  ChannelInfo ci;
  bool haveChan = halowCurrentChannel(ci);

  out.reserve(4096);
  out = "{";

  /* ---- device ---- */
  jsKey(out, "device"); out += '{';
  {
    jsStr(out, "hostname", cfgHostname());
    jsStr(out, "fw_version", FW_VERSION);
    jsNum(out, "uptime_s", (long)(millis() / 1000));
    jsNum(out, "free_heap", (long)ESP.getFreeHeap());
    jsNum(out, "min_free_heap", (long)ESP.getMinFreeHeap());
    size_t psram = ESP.getFreePsram();
    if (psram) jsNum(out, "free_psram", (long)psram); else jsNull(out, "free_psram");

    float t = temperatureRead();
    if (t > -40.0f && t < 150.0f) {
      jsKey(out, "temperature_c");
      char b[16]; snprintf(b, sizeof(b), "%.1f", t); out += b;
    } else {
      jsNull(out, "temperature_c");
    }

    jsStr(out, "wifi_mac", WiFi.softAPmacAddress());
    jsStr(out, "halow_mac", halowOwnMac());
    jsStr(out, "mgmt_ssid", cfgMgmtSsid());
    jsStr(out, "forward_mode", forwardModeName(g_cfg.forwardMode));
    jsNum(out, "mgmt_clients", (long)WiFi.softAPgetStationNum());
    jsStr(out, "reset_reason", resetReasonStr());
  }
  out += '}';

  /* ---- halow configuration ---- */
  jsKey(out, "halow"); out += '{';
  {
    jsStr(out, "role", roleName(g_cfg.role));
    jsStr(out, "region", g_cfg.region);
    if (haveChan) {
      jsNum(out, "channel", ci.chanNum);
      jsNum(out, "centre_freq_hz", (long)ci.centreFreqHz);
      jsNum(out, "bw_mhz", ci.bwMhz);
      jsNum(out, "max_eirp_dbm", ci.maxTxEirpDbm);
      jsNum(out, "s1g_op_class", ci.s1gOpClass);
      jsNum(out, "global_op_class", ci.globalOpClass);
      jsFix2(out, "duty_cycle_pct", ci.dutyCyclePct100);
      /*
       * The MM6108 API has no "read current TX power" call. What we can state
       * truthfully is the configured ceiling: either the user's override or
       * the regulatory maximum for this channel.
       */
      jsNum(out, "tx_power_dbm", g_cfg.txPowerDbm ? g_cfg.txPowerDbm : ci.maxTxEirpDbm);
      jsBool(out, "tx_power_is_override", g_cfg.txPowerDbm != 0);
    } else {
      jsNum(out, "channel", g_cfg.channel);
      jsNull(out, "centre_freq_hz");
      jsNull(out, "bw_mhz");
      jsNull(out, "max_eirp_dbm");
      jsNull(out, "s1g_op_class");
      jsNull(out, "global_op_class");
      jsNull(out, "duty_cycle_pct");
      if (g_cfg.txPowerDbm) jsNum(out, "tx_power_dbm", g_cfg.txPowerDbm);
      else                  jsNull(out, "tx_power_dbm");
      jsBool(out, "tx_power_is_override", g_cfg.txPowerDbm != 0);
    }
    jsStr(out, "ssid", g_cfg.halowSsid);
    jsStr(out, "security", securityName(g_cfg.security));
    jsStr(out, "ip", IPAddress(g_cfg.ip).toString());
    jsStr(out, "netmask", IPAddress(g_cfg.netmask).toString());
    jsStr(out, "gateway", IPAddress(g_cfg.gateway).toString());
    jsStr(out, "peer_ip", IPAddress(g_cfg.peerIp).toString());
    jsStr(out, "mac", halowOwnMac());
    jsStr(out, "peer_mac", g_cfg.role == ROLE_STA ? halowPeerMac() : String(P.mac));

    /*
     * Timing actually programmed into the radio. Both matter a great deal on
     * duty-cycle-limited 1 MHz channels, so they belong on the dashboard.
     */
    if (halowBeaconIntervalTus()) jsNum(out, "beacon_interval_tus", halowBeaconIntervalTus());
    else                          jsNull(out, "beacon_interval_tus");
    if (halowScanDwellMs())       jsNum(out, "scan_dwell_ms", (long)halowScanDwellMs());
    else                          jsNull(out, "scan_dwell_ms");
  }
  out += '}';

  /* ---- link ---- */
  jsKey(out, "link"); out += '{';
  {
    jsBool(out, "up", L.linkUp);
    jsNum(out, "uptime_s", (long)(L.linkUptimeMs / 1000));
    jsNum(out, "disconnects", (long)L.disconnects);
    jsNum(out, "forced_reassoc", (long)halowReassocAttempts());

    if (L.rssiValid) jsNum(out, "rssi_dbm", L.rssiDbm); else jsNull(out, "rssi_dbm");

    /* Not obtainable from this API - stated explicitly rather than guessed. */
    jsNull(out, "snr_db");
    jsNull(out, "noise_dbm");
    jsNull(out, "tx_packets");
    jsNull(out, "rx_packets");
    jsNull(out, "tx_bytes");
    jsNull(out, "rx_bytes");
    jsNull(out, "disconnect_reason");
    jsNull(out, "channel_utilisation");
    jsNull(out, "retries");

    if (L.rateValid) {
      jsNum(out, "mcs", L.mcs);
      jsNum(out, "bw_mhz", L.bwMhz);
      jsBool(out, "short_gi", L.shortGi);
      if (L.phyRateKbps) jsNum(out, "phy_rate_kbps", (long)L.phyRateKbps);
      else               jsNull(out, "phy_rate_kbps");
      jsNum(out, "rate_age_ms", (long)L.rateAgeMs);
    } else {
      jsNull(out, "mcs");
      jsNull(out, "bw_mhz");
      jsNull(out, "short_gi");
      jsNull(out, "phy_rate_kbps");
      jsNull(out, "rate_age_ms");
    }

    if (L.perValid) jsFix2(out, "phy_per_pct", L.perPct100); else jsNull(out, "phy_per_pct");
    jsNum(out, "frames_attempted", (long)L.framesAttempted);
    jsNum(out, "frames_succeeded", (long)L.framesSucceeded);

    if (L.umacValid) {
      jsNum(out, "umac_rssi_dbm", L.umacRssi);
      jsNum(out, "txq_dropped", (long)L.txqDropped);
      jsNum(out, "rxq_dropped", (long)L.rxqDropped);
      jsNum(out, "rx_ccmp_failures", (long)L.rxCcmpFailures);
      jsNum(out, "rx_alloc_failures", (long)L.rxAllocFailures);
      jsNum(out, "rx_reorder_timedout", (long)L.rxReorderTimedout);
      jsNum(out, "hw_restarts", (long)L.hwRestarts);
    } else {
      jsNull(out, "umac_rssi_dbm");
      jsNull(out, "txq_dropped");
      jsNull(out, "rxq_dropped");
      jsNull(out, "rx_ccmp_failures");
      jsNull(out, "rx_alloc_failures");
      jsNull(out, "rx_reorder_timedout");
      jsNull(out, "hw_restarts");
    }
    if (L.dutyValid) jsFix2(out, "duty_cycle_pct", L.dutyCyclePct100);
    else             jsNull(out, "duty_cycle_pct");
  }
  out += '}';

  /* ---- peer ---- */
  jsKey(out, "peer"); out += '{';
  {
    jsBool(out, "valid", P.valid);
    jsBool(out, "fresh", peerIsFresh());
    if (P.valid) {
      jsStr(out, "role", roleName(P.role));
      jsStr(out, "ip", P.ip.toString());
      jsStr(out, "mac", P.mac);
      jsStr(out, "fw", P.fw);
      jsBool(out, "link_up", P.linkUp);
      if (P.rssiValid) jsNum(out, "rssi_dbm", P.rssiDbm); else jsNull(out, "rssi_dbm");
      if (P.rateValid) {
        jsNum(out, "mcs", P.mcs);
        jsNum(out, "bw_mhz", P.bwMhz);
        if (P.phyRateKbps) jsNum(out, "phy_rate_kbps", (long)P.phyRateKbps);
        else               jsNull(out, "phy_rate_kbps");
      } else {
        jsNull(out, "mcs"); jsNull(out, "bw_mhz"); jsNull(out, "phy_rate_kbps");
      }
      jsNum(out, "uptime_s", (long)P.uptimeS);
      jsNum(out, "link_uptime_s", (long)P.linkUptimeS);
      jsNum(out, "free_heap", (long)P.freeHeap);
    }
  }
  out += '}';

  /* ---- rtt ---- */
  jsKey(out, "rtt"); out += '{';
  {
    jsBool(out, "valid", T.valid);
    jsBool(out, "probing", rttProbing());
    jsStr(out, "peer", IPAddress(g_cfg.peerIp).toString());
    if (T.valid) {
      jsFix1(out, "last_ms", T.lastTenthMs);
      jsFix1(out, "avg_ms",  T.avgTenthMs);
      jsFix1(out, "min_ms",  T.minTenthMs == 0xFFFF ? 0 : T.minTenthMs);
      jsFix1(out, "max_ms",  T.maxTenthMs);
    } else {
      jsNull(out, "last_ms"); jsNull(out, "avg_ms");
      jsNull(out, "min_ms");  jsNull(out, "max_ms");
    }
    jsNum(out, "sent", (long)T.sent);
    jsNum(out, "received", (long)T.received);
    jsNum(out, "lost", (long)T.lost);
    if (T.lossPct100 == SAMPLE_NA_U16) jsNull(out, "loss_pct");
    else                               jsFix2(out, "loss_pct", T.lossPct100);
  }
  out += '}';

  /* ---- throughput ---- */
  jsKey(out, "throughput"); out += '{';
  {
    jsNum(out, "state", X.state);
    jsNum(out, "dir", X.dir);
    jsNum(out, "udp", X.udp);
    jsNum(out, "requested_s", X.requestedS);
    jsNum(out, "avg_kbps", (long)X.avgKbps);
    jsNum(out, "current_kbps", (long)X.curKbps);
    jsU64(out, "bytes", X.bytes);
    jsNum(out, "duration_ms", (long)X.durationMs);
    jsStr(out, "local_addr", X.localAddr);
    jsNum(out, "local_port", X.localPort);
    jsStr(out, "remote_addr", X.remoteAddr);
    jsNum(out, "remote_port", X.remotePort);
    if (X.udpStatsValid) {
      jsNum(out, "tx_frames", (long)X.txFrames);
      jsNum(out, "rx_frames", (long)X.rxFrames);
      jsNum(out, "out_of_sequence", (long)X.outOfSeq);
      jsNum(out, "error_count", (long)X.errorCount);
      jsFix2(out, "loss_pct", X.lossPct100);
    } else {
      jsNull(out, "tx_frames"); jsNull(out, "rx_frames");
      jsNull(out, "out_of_sequence"); jsNull(out, "error_count");
      jsNull(out, "loss_pct");
    }
    if (X.ipgValid) jsFix1(out, "mean_ipg_ms", X.meanIpgTenthMs);
    else            jsNull(out, "mean_ipg_ms");
    /* mmiperf does not report TCP retransmissions. */
    jsNull(out, "tcp_retransmissions");
    jsStr(out, "note", X.note);
  }
  out += '}';

  /* ---- radio ---- */
  jsKey(out, "radio"); out += '{';
  {
    jsStr(out, "morselib_version", R.morselibVersion);
    jsStr(out, "morse_fw_version", R.morseFwVersion);
    jsStr(out, "chip_id_string", R.chipIdString);
    if (R.valid) jsNum(out, "chip_id", (long)R.chipId); else jsNull(out, "chip_id");
    if (R.bcfValid) {
      char b[24];
      snprintf(b, sizeof(b), "%u.%u.%u", R.bcfMajor, R.bcfMinor, R.bcfPatch);
      jsStr(out, "bcf_version", b);
      jsStr(out, "bcf_board_desc", R.bcfBoardDesc);
      jsStr(out, "bcf_build_version", R.bcfBuildVersion);
    } else {
      jsNull(out, "bcf_version");
      jsNull(out, "bcf_board_desc");
      jsNull(out, "bcf_build_version");
    }
    jsStr(out, "sdk_version", R.sdkVersion);
    jsStr(out, "regulatory_domain", g_cfg.region);
  }
  out += '}';

  out += '}';
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

static void sendJson(const String &body) {
  s_http.sendHeader("Cache-Control", "no-store");
  s_http.send(200, "application/json", body);
}

static void hStatus(void) {
  String j; buildStatusJson(j); sendJson(j);
}

/*
 * Extracts one top-level object from the status document, so /api/halow and
 * /api/stats return the documented subset rather than the whole thing.
 * The document is flat (one level of nested objects), so brace counting is
 * sufficient and avoids pulling in a JSON parser.
 */
static bool extractSection(const String &doc, const char *name, String &out) {
  String key = String("\"") + name + "\":";
  int k = doc.indexOf(key);
  if (k < 0) return false;
  int start = doc.indexOf('{', k + key.length());
  if (start < 0) return false;

  int depth = 0;
  bool inStr = false, esc = false;
  for (int i = start; i < (int)doc.length(); i++) {
    char c = doc[i];
    if (esc)            { esc = false; continue; }
    if (c == '\\')      { esc = true;  continue; }
    if (c == '"')       { inStr = !inStr; continue; }
    if (inStr)          continue;
    if (c == '{')       depth++;
    else if (c == '}') {
      if (--depth == 0) { out = doc.substring(start, i + 1); return true; }
    }
  }
  return false;
}

static void sendSection(const char *name) {
  String doc;
  buildStatusJson(doc);
  String sec;
  if (extractSection(doc, name, sec)) sendJson(sec);
  else                                sendJson(doc);   /* should not happen */
}

static void hHalow(void) {
  sendSection("halow");
}

static void hHistory(void) {
  uint8_t  win  = (uint8_t)s_http.arg("window").toInt();
  uint16_t mx   = (uint16_t)s_http.arg("max").toInt();
  /* `secs` limits how far back to look; converted to samples for this ring. */
  uint32_t secs = (uint32_t)s_http.arg("secs").toInt();
  if (mx == 0) mx = 200;
  uint16_t tail = 0;
  if (secs) {
    uint32_t n = secs / (win ? HIST_SLOW_DIV : 1);
    tail = (n > 0xFFFF) ? 0xFFFF : (uint16_t)n;
    if (tail == 0) tail = 1;
  }
  String j; histJson(j, win, mx, tail); sendJson(j);
}

static void hRegions(void) {
  String j = "{\"regions\":[";
  for (uint8_t i = 0; i < halowRegionCount(); i++) {
    if (i) j += ',';
    j += '"'; j += halowRegionName(i); j += '"';
  }
  j += "]}";
  sendJson(j);
}

static void hChannels(void) {
  String region = s_http.hasArg("region") ? s_http.arg("region") : String(g_cfg.region);
  String j = "{";
  jsStr(j, "region", region);       /* escaped: this is caller-supplied text */
  j += ",\"channels\":[";
  ChannelInfo ci;
  uint8_t n = halowChannelCount(region.c_str());
  bool first = true;
  for (uint8_t i = 0; i < n; i++) {
    if (!halowChannelAt(region.c_str(), i, ci)) continue;
    if (!first) j += ',';
    first = false;
    j += "{\"channel\":";        j += ci.chanNum;
    j += ",\"centre_freq_hz\":"; j += ci.centreFreqHz;
    j += ",\"bw_mhz\":";         j += ci.bwMhz;
    j += ",\"max_eirp_dbm\":";   j += ci.maxTxEirpDbm;
    j += ",\"duty_cycle_pct100\":"; j += ci.dutyCyclePct100;
    j += ",\"s1g_op_class\":";   j += ci.s1gOpClass;
    j += '}';
  }
  j += "]}";
  sendJson(j);
}

static void hConfigGet(void) {
  String j = "{";
  jsNum(j, "role", g_cfg.role);
  jsStr(j, "halow_ssid", g_cfg.halowSsid);
  jsStr(j, "halow_pass", g_cfg.halowPass);
  jsNum(j, "security", g_cfg.security);
  jsStr(j, "region", g_cfg.region);
  jsNum(j, "channel", g_cfg.channel);
  jsNum(j, "txpower", g_cfg.txPowerDbm);
  jsNum(j, "beacon_tus", g_cfg.beaconIntervalTus);
  jsStr(j, "ip", IPAddress(g_cfg.ip).toString());
  jsStr(j, "netmask", IPAddress(g_cfg.netmask).toString());
  jsStr(j, "gateway", IPAddress(g_cfg.gateway).toString());
  jsStr(j, "peer_ip", IPAddress(g_cfg.peerIp).toString());
  jsStr(j, "mgmt_ssid", g_cfg.mgmtSsid);
  jsStr(j, "mgmt_pass", g_cfg.mgmtPass);
  jsNum(j, "mgmt_channel", g_cfg.mgmtChannel);
  jsNum(j, "forward_mode", g_cfg.forwardMode);
  jsNum(j, "iperf_port", g_cfg.iperfPort);
  jsNum(j, "rtt_port", g_cfg.rttPort);
  jsNum(j, "peer_port", g_cfg.peerPort);
  j += '}';
  sendJson(j);
}

static bool argIp(const char *name, uint32_t &dst) {
  if (!s_http.hasArg(name)) return true;
  IPAddress a;
  if (!a.fromString(s_http.arg(name))) return false;
  dst = (uint32_t)a;
  return true;
}

static void argStr(const char *name, char *dst, size_t len) {
  if (s_http.hasArg(name)) strlcpy(dst, s_http.arg(name).c_str(), len);
}

/* Emits {"ok":false,"error":"..."} with the message properly escaped. */
static void sendError(const String &err) {
  String j = "{\"ok\":false";
  jsStr(j, "error", err);
  j += '}';
  sendJson(j);
}

static void hConfigPost(void) {
  String err;

  /*
   * Validate against a copy and commit only if everything passed, so a
   * rejected request cannot leave half of its changes live in g_cfg.
   */
  AppConfig saved = g_cfg;

  if (s_http.hasArg("role")) {
    int r = s_http.arg("role").toInt();
    if (r == ROLE_AP || r == ROLE_STA) g_cfg.role = (uint8_t)r;
  }

  argStr("halow_ssid", g_cfg.halowSsid, sizeof(g_cfg.halowSsid));
  argStr("halow_pass", g_cfg.halowPass, sizeof(g_cfg.halowPass));
  if (s_http.hasArg("security")) g_cfg.security = (uint8_t)s_http.arg("security").toInt();

  if (s_http.hasArg("region")) {
    String r = s_http.arg("region");
    if (!halowRegionExists(r.c_str())) {
      err = "unknown region \"" + r + "\"";
    } else {
      strlcpy(g_cfg.region, r.c_str(), sizeof(g_cfg.region));
    }
  }
  if (err.length() == 0 && s_http.hasArg("channel")) {
    uint8_t ch = (uint8_t)s_http.arg("channel").toInt();
    ChannelInfo ci;
    if (!halowChannelByNumber(g_cfg.region, ch, ci)) {
      err = "channel " + String(ch) + " is not valid for region " + String(g_cfg.region);
    } else {
      g_cfg.channel = ch;
    }
  }
  if (s_http.hasArg("txpower")) {
    long p = s_http.arg("txpower").toInt();
    if (p < 0 || p > 30) err = "TX power must be 0..30 dBm";
    else g_cfg.txPowerDbm = (uint16_t)p;
  }
  if (s_http.hasArg("beacon_tus")) {
    long b = s_http.arg("beacon_tus").toInt();
    if (b != 0 && (b < 50 || b > 10000)) err = "beacon interval must be 0 (auto) or 50..10000 TU";
    else g_cfg.beaconIntervalTus = (uint16_t)b;
  }

  if (!argIp("ip", g_cfg.ip))           err = "invalid IP address";
  if (!argIp("netmask", g_cfg.netmask)) err = "invalid netmask";
  if (!argIp("gateway", g_cfg.gateway)) err = "invalid gateway";
  if (!argIp("peer_ip", g_cfg.peerIp))  err = "invalid peer IP";

  argStr("mgmt_ssid", g_cfg.mgmtSsid, sizeof(g_cfg.mgmtSsid));
  if (s_http.hasArg("mgmt_pass")) {
    String p = s_http.arg("mgmt_pass");
    if (p.length() > 0 && p.length() < 8) err = "management password must be at least 8 characters";
    else strlcpy(g_cfg.mgmtPass, p.c_str(), sizeof(g_cfg.mgmtPass));
  }
  if (s_http.hasArg("forward_mode")) {
    long f = s_http.arg("forward_mode").toInt();
    if (f < 0 || f > 2) err = "forward mode must be 0 (isolated), 1 (NAT) or 2 (route)";
    else g_cfg.forwardMode = (uint8_t)f;
  }
  if (s_http.hasArg("mgmt_channel")) {
    long c = s_http.arg("mgmt_channel").toInt();
    if (c < 1 || c > 13) err = "management channel must be 1..13";
    else g_cfg.mgmtChannel = (uint8_t)c;
  }

  if (s_http.hasArg("iperf_port")) g_cfg.iperfPort = (uint16_t)s_http.arg("iperf_port").toInt();
  if (s_http.hasArg("rtt_port"))   g_cfg.rttPort   = (uint16_t)s_http.arg("rtt_port").toInt();
  if (s_http.hasArg("peer_port"))  g_cfg.peerPort  = (uint16_t)s_http.arg("peer_port").toInt();

  if (err.length()) {
    g_cfg = saved;   /* discard every partial change */
    LOGW("WEB", "config rejected: %s", err.c_str());
    sendError(err);
    return;
  }

  bool ok = cfgSave();
  bool reboot = s_http.hasArg("reboot");
  sendJson(String("{\"ok\":") + (ok ? "true" : "false") +
           ",\"reboot\":" + (reboot ? "true" : "false") + "}");
  if (ok && reboot) {
    LOGI("WEB", "reboot requested after configuration change");
    s_rebootAt = millis() + 600;
  }
}

static void hTestStart(void) {
  uint8_t  dir  = s_http.arg("dir") == "rx" ? THR_DIR_RX : THR_DIR_TX;
  bool     udp  = s_http.arg("proto") == "udp";
  uint16_t durS = (uint16_t)s_http.arg("duration").toInt();
  uint32_t rate = (uint32_t)s_http.arg("rate").toInt();
  uint16_t pkt  = (uint16_t)s_http.arg("packet").toInt();

  String err;
  if (thrStartTest(dir, udp, durS, rate, pkt, err)) sendJson("{\"ok\":true}");
  else                                              sendError(err);
}

static void hTestStop(void) {
  thrAbort();
  sendJson("{\"ok\":true}");
}

static void hPingConfig(void) {
  bool en = s_http.arg("enable") == "1";
  uint16_t iv = (uint16_t)s_http.arg("interval").toInt();
  if (iv == 0) iv = g_cfg.contIntervalMs;
  rttSetProbing(en, iv);
  g_cfg.contEnabled    = en ? 1 : 0;
  g_cfg.contIntervalMs = iv;
  cfgSave();
  sendJson("{\"ok\":true}");
}

static void hPingReset(void) {
  rttResetStats();
  sendJson("{\"ok\":true}");
}

static void hStatsReset(void) {
  linkMonitorReset();
  rttResetStats();
  sendJson("{\"ok\":true}");
}

static void hReboot(void) {
  sendJson("{\"ok\":true}");
  LOGW("WEB", "reboot requested via API");
  s_rebootAt = millis() + 500;
}

static void hLogTxt(void) {
  String t; logDumpText(t);
  s_http.sendHeader("Cache-Control", "no-store");
  s_http.send(200, "text/plain; charset=utf-8", t);
}

static void hLogJson(void) {
  String j; logDumpJson(j); sendJson(j);
}

static void hLogClear(void) {
  logClear();
  sendJson("{\"ok\":true}");
}

/*
 * Both exports are streamed in batches. A full ring is ~80 KB, and building
 * that as one String risks a failed reserve() - which String silently turns
 * into dropped appends, i.e. a truncated file served with a 200.
 */
#define EXPORT_BATCH 60

static void hExportCsv(void) {
  uint8_t  win   = (uint8_t)s_http.arg("window").toInt();
  uint16_t total = histCount(win);

  s_http.sendHeader("Content-Disposition",
                    "attachment; filename=\"halow-test-" + cfgHostname() + ".csv\"");
  s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_http.send(200, "text/csv", "");

  String chunk;
  histCsvHeader(chunk);
  s_http.sendContent(chunk);

  for (uint16_t i = 0; i < total; i += EXPORT_BATCH) {
    chunk = "";
    histCsvRowsRange(chunk, win, i, EXPORT_BATCH);
    s_http.sendContent(chunk);
  }
  s_http.sendContent("");
}

static void hExportJson(void) {
  uint8_t  win   = (uint8_t)s_http.arg("window").toInt();
  uint16_t total = histCount(win);

  /* buildStatusJson() assigns to its argument, so build it separately. */
  String meta;
  buildStatusJson(meta);

  s_http.sendHeader("Content-Disposition",
                    "attachment; filename=\"halow-test-" + cfgHostname() + ".json\"");
  s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_http.send(200, "application/json", "");

  String head = "{\"meta\":";
  head += meta;
  head += ",\"history\":{\"window\":";
  head += win;
  head += ",\"step_s\":";
  head += (win ? HIST_SLOW_DIV : 1);
  head += ",\"points\":[";
  s_http.sendContent(head);

  String chunk;
  for (uint16_t i = 0; i < total; i += EXPORT_BATCH) {
    chunk = "";
    histJsonPointsRange(chunk, win, i, EXPORT_BATCH);
    s_http.sendContent(chunk);
  }
  s_http.sendContent("]}}");
  s_http.sendContent("");
}

static void hStats(void) {
  sendSection("link");
}

static void sendProgmem(const char *body, const char *type) {
  s_http.sendHeader("Cache-Control", "max-age=600");
  s_http.send_P(200, type, body);
}

/* ------------------------------------------------------------------ */
/* SSE                                                                 */
/* ------------------------------------------------------------------ */

static void sseAccept(void) {
  while (s_sse.hasClient()) {
    WiFiClient c = s_sse.accept();
    if (!c) break;

    int slot = -1;
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
      if (!s_sseClients[i] || !s_sseClients[i].connected()) { slot = i; break; }
    }
    if (slot < 0) { c.stop(); continue; }

    s_sseClients[slot] = c;
    s_sseClients[slot].print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: keep-alive\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "\r\n"
      "retry: 3000\r\n\r\n");
  }
}

static void ssePush(void) {
  bool any = false;
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (s_sseClients[i] && s_sseClients[i].connected()) { any = true; break; }
  }
  if (!any) return;

  String j; buildStatusJson(j);
  for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
    if (!s_sseClients[i] || !s_sseClients[i].connected()) continue;
    s_sseClients[i].print("data: ");
    s_sseClients[i].print(j);
    s_sseClients[i].print("\n\n");
  }
}

/* ------------------------------------------------------------------ */

void webInit(void) {
  WiFi.mode(WIFI_AP);
  WiFi.softAPsetHostname(cfgHostname().c_str());

  /*
   * DHCP options have to be set while the server is stopped, i.e. before
   * softAP() brings it up. In ISOLATED mode we withhold the router and DNS
   * options, so panel clients get an address but no default route and their
   * traffic never reaches the HaLow link.
   */
  esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apNetif) {
    dhcps_offer_t offerRouter = (g_cfg.forwardMode == FWD_ISOLATED) ? 0 : OFFER_ROUTER;
    dhcps_offer_t offerDns    = (g_cfg.forwardMode == FWD_ISOLATED) ? 0 : OFFER_DNS;
    esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET,
                           ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                           &offerRouter, sizeof(offerRouter));
    esp_netif_dhcps_option(apNetif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offerDns, sizeof(offerDns));
  }

  String ssid = cfgMgmtSsid();
  const char *pass = (strlen(g_cfg.mgmtPass) >= 8) ? g_cfg.mgmtPass : NULL;
  bool ok = WiFi.softAP(ssid.c_str(), pass, g_cfg.mgmtChannel);

  if (ok) {
    LOGI("WEB", "management AP \"%s\" up on channel %u, panel at http://%s/",
         ssid.c_str(), g_cfg.mgmtChannel, WiFi.softAPIP().toString().c_str());
  } else {
    LOGE("WEB", "failed to start management AP");
  }

  s_http.on("/",           HTTP_GET, []{ sendProgmem(INDEX_HTML, "text/html"); });
  s_http.on("/index.html", HTTP_GET, []{ sendProgmem(INDEX_HTML, "text/html"); });
  s_http.on("/style.css",  HTTP_GET, []{ sendProgmem(APP_CSS, "text/css"); });
  s_http.on("/app.js",     HTTP_GET, []{ sendProgmem(APP_JS, "application/javascript"); });

  s_http.on("/api/status",  HTTP_GET, hStatus);
  s_http.on("/api/halow",   HTTP_GET, hHalow);
  s_http.on("/api/stats",   HTTP_GET, hStats);
  s_http.on("/api/history", HTTP_GET, hHistory);
  s_http.on("/api/regions", HTTP_GET, hRegions);
  s_http.on("/api/channels",HTTP_GET, hChannels);

  s_http.on("/api/config", HTTP_GET,  hConfigGet);
  s_http.on("/api/config", HTTP_POST, hConfigPost);

  s_http.on("/api/test/start", HTTP_POST, hTestStart);
  s_http.on("/api/test/stop",  HTTP_POST, hTestStop);
  s_http.on("/api/ping/config",HTTP_POST, hPingConfig);
  s_http.on("/api/ping/reset", HTTP_POST, hPingReset);
  s_http.on("/api/stats/reset",HTTP_POST, hStatsReset);
  s_http.on("/api/reboot",     HTTP_POST, hReboot);

  s_http.on("/api/log.txt",  HTTP_GET,  hLogTxt);
  s_http.on("/api/log.json", HTTP_GET,  hLogJson);
  s_http.on("/api/log/clear",HTTP_POST, hLogClear);

  s_http.on("/api/export.csv",  HTTP_GET, hExportCsv);
  s_http.on("/api/export.json", HTTP_GET, hExportJson);

  s_http.onNotFound([]{ s_http.send(404, "text/plain", "not found"); });

  s_http.begin();
  s_sse.begin();
  s_sse.setNoDelay(true);
  LOGI("WEB", "HTTP server on port 80, SSE stream on port %u", SSE_PORT);

  /*
   * Let a phone on the management Wi-Fi reach the *other* node's panel at its
   * HaLow address (e.g. http://192.168.50.1/ while connected to this node).
   *
   * Routing there already works - the HaLow subnet is directly connected - but
   * the far node has no route back to 192.168.4.x, so replies would be
   * dropped. NAPT rewrites the source to our HaLow address, which the far node
   * can answer. Only forwarded traffic is affected; the local panel is
   * untouched, and the default route is deliberately left alone.
   */
  if (apNetif) {
    switch (g_cfg.forwardMode) {
      case FWD_NAT: {
        esp_err_t err = esp_netif_napt_enable(apNetif);
        if (err == ESP_OK) {
          LOGI("WEB", "forwarding: NAT - the peer's panel is reachable at http://%s/",
               IPAddress(g_cfg.peerIp).toString().c_str());
        } else {
          LOGW("WEB", "esp_netif_napt_enable() failed (0x%x)", err);
        }
        break;
      }
      case FWD_ROUTE:
        esp_netif_napt_disable(apNetif);
        LOGI("WEB", "forwarding: ROUTE (no NAT) - upstream needs a route back to %s/24",
             WiFi.softAPIP().toString().c_str());
        break;
      default:
        esp_netif_napt_disable(apNetif);
        LOGI("WEB", "forwarding: ISOLATED - panel clients get no default route, so "
                    "they cannot disturb the measurements");
        break;
    }
  }
}

void webTick(void) {
  s_http.handleClient();
  sseAccept();

  uint32_t now = millis();
  if ((uint32_t)(now - s_lastSse) >= SSE_PERIOD_MS) {
    s_lastSse = now;
    ssePush();
  }

  if (s_rebootAt && (int32_t)(now - s_rebootAt) >= 0) {
    Serial.flush();
    ESP.restart();
  }
}
