#include "serial_cli.h"
#include "config.h"
#include "logbuf.h"
#include "halow_manager.h"
#include "link_monitor.h"
#include "rtt_test.h"
#include "peer_link.h"
#include "throughput_test.h"

#include <HaLow.h>
#include "mmwlan.h"

#define CLI_BUF 128

static char     s_buf[CLI_BUF];
static uint8_t  s_len = 0;

static void printHelp(void) {
  Serial.println(F(
    "\r\ncommands:\r\n"
    "  help                       this list\r\n"
    "  status                     live link metrics\r\n"
    "  cfg                        current configuration\r\n"
    "  chans                      channel table for the configured region\r\n"
    "  role ap|sta                set role\r\n"
    "  region <XX>                e.g. EU\r\n"
    "  chan <n>                   S1G channel number\r\n"
    "  ssid <s> / pass <s>        HaLow credentials\r\n"
    "  ip|mask|gw|peer <addr>     HaLow addressing\r\n"
    "  txpower <dBm>              0 = regulatory max\r\n"
    "  beacon <TU>                AP beacon interval, 0 = auto (EU 1 MHz needs ~300)\r\n"
    "  save                       write config to NVS\r\n"
    "  reboot                     restart\r\n"
    "  factory                    reset config to defaults\r\n"
    "  ping on|off [ms]           continuous RTT probe\r\n"
    "  test tcp|udp tx|rx [s] [kbps]   throughput test\r\n"
    "  stop                       stop tracking the current test\r\n"));
}

static void printCfg(void) {
  Serial.printf("role      : %s\r\n", roleName(g_cfg.role));
  Serial.printf("region    : %s   channel %u\r\n", g_cfg.region, g_cfg.channel);
  ChannelInfo ci;
  if (halowChannelByNumber(g_cfg.region, g_cfg.channel, ci)) {
    Serial.printf("channel   : %lu.%lu MHz, %u MHz BW, max %d dBm EIRP\r\n",
                  (unsigned long)(ci.centreFreqHz / 1000000UL),
                  (unsigned long)((ci.centreFreqHz % 1000000UL) / 100000UL),
                  ci.bwMhz, (int)ci.maxTxEirpDbm);
  }
  Serial.printf("ssid      : %s (%s)\r\n", g_cfg.halowSsid, securityName(g_cfg.security));
  Serial.printf("passphrase: %s (%u chars)\r\n",
                g_cfg.halowPass[0] ? "set" : "EMPTY -> link will be OPEN",
                (unsigned)strlen(g_cfg.halowPass));
  Serial.printf("ip        : %s  mask %s  gw %s\r\n",
                IPAddress(g_cfg.ip).toString().c_str(),
                IPAddress(g_cfg.netmask).toString().c_str(),
                IPAddress(g_cfg.gateway).toString().c_str());
  Serial.printf("peer      : %s\r\n", IPAddress(g_cfg.peerIp).toString().c_str());
  Serial.printf("txpower   : %u dBm (0 = regulatory max)\r\n", g_cfg.txPowerDbm);
  Serial.printf("beacon    : %u TU%s\r\n", g_cfg.beaconIntervalTus,
                g_cfg.beaconIntervalTus ? "" : " (auto)");
  Serial.printf("mgmt ssid : %s\r\n", cfgMgmtSsid().c_str());
}

static void printStatus(void) {
  const LinkStats &L = linkStats();
  const PeerInfo  &P = peerData();
  const RttStats  &T = rttStats();
  const ThroughputResult &X = thrResult();

  Serial.printf("link      : %s", L.linkUp ? "UP" : "DOWN");
  if (L.linkUp) Serial.printf("  uptime %lus", (unsigned long)(L.linkUptimeMs / 1000));
  Serial.printf("  disconnects %lu\r\n", (unsigned long)L.disconnects);

  if (L.rssiValid) Serial.printf("rssi      : %d dBm\r\n", L.rssiDbm);
  else             Serial.println(F("rssi      : n/a (STA-side measurement)"));

  if (L.rateValid) {
    Serial.printf("rate      : MCS%d  %u MHz  %s  ", L.mcs, L.bwMhz,
                  L.shortGi ? "SGI" : "LGI");
    if (L.phyRateKbps) Serial.printf("%lu.%02lu Mbps\r\n",
                                     (unsigned long)(L.phyRateKbps / 1000),
                                     (unsigned long)((L.phyRateKbps % 1000) / 10));
    else Serial.println(F("PHY rate undefined"));
  } else {
    Serial.println(F("rate      : n/a (no traffic since last sample)"));
  }
  if (L.perValid) Serial.printf("phy PER   : %u.%02u %%\r\n", L.perPct100/100, L.perPct100%100);
  Serial.printf("frames    : %lu attempted, %lu succeeded\r\n",
                (unsigned long)L.framesAttempted, (unsigned long)L.framesSucceeded);

  if (T.valid) {
    Serial.printf("rtt       : last %u.%u  avg %u.%u  min %u.%u  max %u.%u ms\r\n",
                  T.lastTenthMs/10, T.lastTenthMs%10, T.avgTenthMs/10, T.avgTenthMs%10,
                  T.minTenthMs/10, T.minTenthMs%10, T.maxTenthMs/10, T.maxTenthMs%10);
  } else {
    Serial.println(F("rtt       : no replies yet"));
  }
  Serial.printf("probes    : sent %lu  recv %lu  lost %lu\r\n",
                (unsigned long)T.sent, (unsigned long)T.received, (unsigned long)T.lost);

  Serial.printf("peer      : %s", P.valid ? (peerIsFresh() ? "live" : "stale") : "no data");
  if (P.valid) {
    Serial.printf("  %s  role %s  mac %s", P.ip.toString().c_str(), roleName(P.role), P.mac);
    if (P.rssiValid) Serial.printf("  rssi %d dBm", P.rssiDbm);
    if (P.rateValid) Serial.printf("  MCS%d", P.mcs);
  }
  Serial.println();

  Serial.printf("iperf     : state %u  avg %lu kbps  cur %lu kbps  bytes %llu\r\n",
                X.state, (unsigned long)X.avgKbps, (unsigned long)X.curKbps,
                (unsigned long long)X.bytes);
  if (X.udpStatsValid) {
    Serial.printf("iperf udp : tx %lu  rx %lu  lost %lu  ooo %lu  loss %u.%02u %%\r\n",
                  (unsigned long)X.txFrames, (unsigned long)X.rxFrames,
                  (unsigned long)X.errorCount, (unsigned long)X.outOfSeq,
                  X.lossPct100/100, X.lossPct100%100);
  }
  if (X.note[0]) Serial.printf("iperf note: %s\r\n", X.note);
  Serial.printf("heap      : %lu B free\r\n", (unsigned long)ESP.getFreeHeap());
}

static void printChans(void) {
  ChannelInfo ci;
  uint8_t n = halowChannelCount(g_cfg.region);
  Serial.printf("region %s, %u channels:\r\n", g_cfg.region, n);
  for (uint8_t i = 0; i < n; i++) {
    if (!halowChannelAt(g_cfg.region, i, ci)) continue;
    Serial.printf("  ch %-3u %lu.%lu MHz  %u MHz  max %d dBm  duty %u.%02u %%\r\n",
                  ci.chanNum,
                  (unsigned long)(ci.centreFreqHz / 1000000UL),
                  (unsigned long)((ci.centreFreqHz % 1000000UL) / 100000UL),
                  ci.bwMhz, (int)ci.maxTxEirpDbm,
                  ci.dutyCyclePct100 / 100, ci.dutyCyclePct100 % 100);
  }
}

/* Returns the argument after the first space, or "" if there is none. */
static const char *argOf(char *line) {
  char *sp = strchr(line, ' ');
  if (!sp) return "";
  *sp = '\0';
  return sp + 1;
}

static bool setIp(uint32_t &dst, const char *s) {
  IPAddress a;
  if (!a.fromString(s)) { Serial.println(F("bad address")); return false; }
  dst = (uint32_t)a;
  return true;
}

static void execute(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  const char *arg = argOf(line);

  if (!strcmp(line, "help") || !strcmp(line, "?")) { printHelp(); return; }
  if (!strcmp(line, "status")) { printStatus(); return; }
  if (!strcmp(line, "cfg"))    { printCfg(); return; }
  if (!strcmp(line, "chans"))  { printChans(); return; }

  if (!strcmp(line, "role")) {
    if (!strcmp(arg, "ap"))       g_cfg.role = ROLE_AP;
    else if (!strcmp(arg, "sta")) g_cfg.role = ROLE_STA;
    else { Serial.println(F("usage: role ap|sta")); return; }
    Serial.printf("role = %s (save + reboot to apply)\r\n", roleName(g_cfg.role));
    return;
  }
  if (!strcmp(line, "region")) {
    if (!halowRegionExists(arg)) { Serial.println(F("unknown region")); return; }
    strlcpy(g_cfg.region, arg, sizeof(g_cfg.region));
    Serial.printf("region = %s\r\n", g_cfg.region);
    return;
  }
  if (!strcmp(line, "chan")) {
    uint8_t ch = (uint8_t)atoi(arg);
    ChannelInfo ci;
    if (!halowChannelByNumber(g_cfg.region, ch, ci)) {
      Serial.printf("channel %u invalid for region %s\r\n", ch, g_cfg.region);
      return;
    }
    g_cfg.channel = ch;
    Serial.printf("channel = %u (%lu.%lu MHz, %u MHz)\r\n", ch,
                  (unsigned long)(ci.centreFreqHz / 1000000UL),
                  (unsigned long)((ci.centreFreqHz % 1000000UL) / 100000UL), ci.bwMhz);
    return;
  }
  if (!strcmp(line, "ssid")) { strlcpy(g_cfg.halowSsid, arg, sizeof(g_cfg.halowSsid)); Serial.println(F("ok")); return; }
  if (!strcmp(line, "pass")) { strlcpy(g_cfg.halowPass, arg, sizeof(g_cfg.halowPass)); Serial.println(F("ok")); return; }
  if (!strcmp(line, "ip"))   { if (setIp(g_cfg.ip, arg))      Serial.println(F("ok")); return; }
  if (!strcmp(line, "mask")) { if (setIp(g_cfg.netmask, arg)) Serial.println(F("ok")); return; }
  if (!strcmp(line, "gw"))   { if (setIp(g_cfg.gateway, arg)) Serial.println(F("ok")); return; }
  if (!strcmp(line, "peer")) { if (setIp(g_cfg.peerIp, arg))  Serial.println(F("ok")); return; }
  if (!strcmp(line, "txpower")) {
    long p = atol(arg);
    if (p < 0 || p > 30) { Serial.println(F("0..30 dBm")); return; }
    g_cfg.txPowerDbm = (uint16_t)p; Serial.println(F("ok")); return;
  }
  if (!strcmp(line, "beacon")) {
    long b = atol(arg);
    if (b < 0 || b > 10000) { Serial.println(F("0 (auto) or 50..10000 TU")); return; }
    g_cfg.beaconIntervalTus = (uint16_t)b;
    Serial.printf("beacon interval = %ld TU%s\r\n", b, b ? "" : " (auto)");
    return;
  }
  if (!strcmp(line, "save"))    { Serial.println(cfgSave() ? F("saved") : F("save failed")); return; }
  if (!strcmp(line, "factory")) { cfgFactoryReset(); cfgSave(); Serial.println(F("defaults restored")); return; }
  if (!strcmp(line, "reboot"))  { Serial.println(F("rebooting")); Serial.flush(); delay(100); ESP.restart(); return; }

  if (!strcmp(line, "ping")) {
    char *iv = strchr((char *)arg, ' ');
    uint16_t ms = g_cfg.contIntervalMs;
    if (iv) { *iv = '\0'; ms = (uint16_t)atoi(iv + 1); }
    bool on = !strcmp(arg, "on");
    rttSetProbing(on, ms ? ms : 1000);
    g_cfg.contEnabled = on ? 1 : 0;
    if (ms) g_cfg.contIntervalMs = ms;
    Serial.printf("probing %s\r\n", on ? "on" : "off");
    return;
  }

  if (!strcmp(line, "stop")) { thrAbort(); Serial.println(F("ok")); return; }

  if (!strcmp(line, "scan")) {
    /* Optional dwell time per channel in ms; EU duty cycling can space beacons out. */
    uint32_t dwell = (uint32_t)atol(arg);
    if (dwell < 100) dwell = 300;
    Serial.printf("scanning (%lu ms/channel)...\r\n", (unsigned long)dwell);
    int16_t n = HaLow.scanNetworks(false, false, false, dwell);
    Serial.printf("%d network(s)\r\n", (int)n);
    for (int16_t i = 0; i < n; i++) {
      Serial.printf("  \"%s\"  ch %ld  rssi %ld dBm  %s  %s\r\n",
                    HaLow.SSID(i).c_str(), (long)HaLow.channel(i), (long)HaLow.RSSI(i),
                    HaLow.BSSIDstr(i).c_str(), HaLow.encryptionTypeStr(i).c_str());
    }
    HaLow.scanDelete();
    return;
  }

  if (!strcmp(line, "state")) {
    Serial.printf("role             : %s\r\n", roleName(g_cfg.role));
    Serial.printf("HaLow.status()   : %d (WL_CONNECTED=%d)\r\n",
                  (int)HaLow.status(), (int)WL_CONNECTED);
    Serial.printf("HaLow.localIP()  : %s\r\n", HaLow.localIP().toString().c_str());
    Serial.println(F("mmwlan_status: 0=SUCCESS 1=ERROR 3=UNAVAILABLE 10=NOT_RUNNING"));

    uint8_t b[6] = {0};
    if (g_cfg.role == ROLE_AP) {
      enum mmwlan_status st = mmwlan_ap_get_bssid(b);
      Serial.printf("ap_get_bssid     : %d", (int)st);
      if (st == MMWLAN_SUCCESS)
        Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X", b[0],b[1],b[2],b[3],b[4],b[5]);
      Serial.println();
      Serial.printf("AP isEnabled     : %d  chan %u  opclass %u\r\n",
                    (int)HaLow.HalowAPClass::isEnabled(),
                    HaLow.HalowAPClass::getChannel(), HaLow.HalowAPClass::getOpClass());
    } else {
      Serial.printf("mmwlan sta state : %d (0=DISABLED 1=CONNECTING 2=CONNECTED)\r\n",
                    (int)mmwlan_get_sta_state());
      Serial.printf("get_bssid        : %d\r\n", (int)mmwlan_get_bssid(b));
    }

    uint8_t m[6] = {0};
    enum mmwlan_vif vif = (g_cfg.role == ROLE_AP) ? MMWLAN_VIF_AP : MMWLAN_VIF_STA;
    if (mmwlan_get_vif_mac_addr(vif, m) == MMWLAN_SUCCESS)
      Serial.printf("own mac          : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                    m[0],m[1],m[2],m[3],m[4],m[5]);

    struct mmwlan_duty_cycle_stats d;
    if (mmwlan_get_duty_cycle_stats(&d) == MMWLAN_SUCCESS)
      Serial.printf("duty cycle       : %lu (1/100 %%), mode %d\r\n",
                    (unsigned long)d.duty_cycle, (int)d.mode);
    return;
  }

  if (!strcmp(line, "test")) {
    /* test <tcp|udp> <tx|rx> [seconds] [kbps] */
    char proto[8] = {0}, dir[8] = {0};
    int secs = 10; unsigned long kbps = 0;
    int n = sscanf(arg, "%7s %7s %d %lu", proto, dir, &secs, &kbps);
    if (n < 2) { Serial.println(F("usage: test tcp|udp tx|rx [s] [kbps]")); return; }
    bool udp = !strcmp(proto, "udp");
    uint8_t d = !strcmp(dir, "rx") ? THR_DIR_RX : THR_DIR_TX;
    String err;
    if (thrStartTest(d, udp, (uint16_t)secs, (uint32_t)kbps, 0, err)) {
      Serial.printf("started %s %s for %d s\r\n", udp ? "UDP" : "TCP", dir, secs);
    } else {
      Serial.printf("failed: %s\r\n", err.c_str());
    }
    return;
  }

  Serial.printf("unknown command \"%s\" (try help)\r\n", line);
}

void cliInit(void) {
  s_len = 0;
  Serial.println(F("\r\nserial console ready - type 'help'"));
}

void cliTick(void) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      s_buf[s_len] = '\0';
      if (s_len) execute(s_buf);
      s_len = 0;
      Serial.print("> ");
      continue;
    }
    if (c == 8 || c == 127) { if (s_len) s_len--; continue; }
    if (s_len < CLI_BUF - 1) s_buf[s_len++] = c;
  }
}
