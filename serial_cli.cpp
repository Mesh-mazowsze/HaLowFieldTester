#include "serial_cli.h"
#include "config.h"
#include "logbuf.h"
#include "halow_manager.h"
#include "link_monitor.h"
#include "rtt_test.h"
#include "peer_link.h"
#include "throughput_test.h"

#include "web_server.h"
#include "stats.h"

#include <Wire.h>
#include <WiFi.h>
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
    "  stop                       stop tracking the current test\r\n"
    "  json [history]             dump the document the web panel consumes\r\n"
    "  httpget [ip]               fetch /api/status from the peer over HaLow\r\n"
    "  scan [ms]                  HaLow scan (dwell per channel)\r\n"
    "  i2cscan [sda scl]          hunt for an I2C display on an add-on board\r\n"
    "  btnscan [s]                identify button GPIOs on an add-on board\r\n"));
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
  Serial.printf("forwarding: %s\r\n", forwardModeName(g_cfg.forwardMode));
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

/*
 * ---------------------------------------------------------------------------
 * Hardware discovery helpers, for identifying an add-on board (display/buttons)
 * whose pinout is not documented.
 *
 * Pins that must never be touched on the HT-RC3268:
 *   1, 3, 8, 9, 10, 11, 12, 13, 14  MM6108 (SPI, RESET, WAKE, BUSY, LDO)
 *   19, 20                          USB D-/D+ (would kill the USB console)
 *   26..37                          SPI flash and OPI PSRAM
 *   43, 44                          UART0
 */
static const uint8_t kSafePins[] = {
  0, 2, 4, 5, 6, 7, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47, 48
};
#define SAFE_PIN_COUNT (sizeof(kSafePins) / sizeof(kSafePins[0]))

static bool pinIsSafe(uint8_t p) {
  for (size_t i = 0; i < SAFE_PIN_COUNT; i++) if (kSafePins[i] == p) return true;
  return false;
}

/* Candidate (SDA, SCL) pairs to try when hunting for an I2C display. */
static const uint8_t kI2cPairs[][2] = {
  {  5,  6 },   /* the variant's declared SDA/SCL */
  {  6,  5 },
  { 17, 18 },
  { 41, 42 },
  { 40, 39 },
  { 47, 48 },
  { 45, 46 },
  {  4,  7 },
  {  2, 15 },
  { 21, 16 },
};
#define I2C_PAIR_COUNT (sizeof(kI2cPairs) / sizeof(kI2cPairs[0]))

static const char *i2cGuess(uint8_t addr) {
  switch (addr) {
    case 0x3C: case 0x3D: return "SSD1306 / SH1106 OLED";
    case 0x27: case 0x3F: return "PCF8574 (LCD backpack)";
    case 0x76: case 0x77: return "BMP/BME280";
    case 0x68:            return "RTC / IMU";
    case 0x20: case 0x21: return "MCP23017 / PCF8574 GPIO expander";
    default:              return "unknown";
  }
}

static void scanI2cPair(uint8_t sda, uint8_t scl) {
  Wire.end();
  if (!Wire.begin((int)sda, (int)scl, 100000)) {
    Serial.printf("  SDA=%-2u SCL=%-2u : begin() failed\r\n", sda, scl);
    return;
  }

  uint8_t addrs[16];
  uint8_t found = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (found < sizeof(addrs)) addrs[found] = a;
      found++;
    }
  }
  Wire.end();

  /*
   * Every address acknowledging is not 112 chips - it means the bus is stuck:
   * SDA held low, or SDA and SCL shorted together. Report the fault rather
   * than a page of nonsense.
   */
  if (found > 12) {
    Serial.printf("  SDA=%-2u SCL=%-2u : BUS FAULT - all %u addresses ACK "
                  "(SDA stuck low or SDA/SCL shorted; not I2C here)\r\n",
                  sda, scl, found);
    return;
  }
  if (!found) {
    Serial.printf("  SDA=%-2u SCL=%-2u : -\r\n", sda, scl);
    return;
  }
  for (uint8_t i = 0; i < found; i++) {
    Serial.printf("  SDA=%-2u SCL=%-2u : device 0x%02X  (%s)\r\n",
                  sda, scl, addrs[i], i2cGuess(addrs[i]));
  }
}

/*
 * Bit-banged I2C probe.
 *
 * The Wire driver reported an ACK from all 112 addresses on some pin pairs,
 * which is not believable. This drives the bus by hand so the ACK bit is read
 * directly and cannot be masked by the peripheral's own error handling.
 * Lines are open-drain: driven low, or released to the external pull-up.
 */
#define BB_DELAY_US 5

static inline void bbRelease(uint8_t p) { pinMode(p, INPUT_PULLUP); }
static inline void bbLow(uint8_t p)     { pinMode(p, OUTPUT); digitalWrite(p, LOW); }

static void bbStart(uint8_t sda, uint8_t scl) {
  bbRelease(sda); bbRelease(scl); delayMicroseconds(BB_DELAY_US);
  bbLow(sda);     delayMicroseconds(BB_DELAY_US);
  bbLow(scl);     delayMicroseconds(BB_DELAY_US);
}

static void bbStop(uint8_t sda, uint8_t scl) {
  bbLow(sda);     delayMicroseconds(BB_DELAY_US);
  bbRelease(scl); delayMicroseconds(BB_DELAY_US);
  bbRelease(sda); delayMicroseconds(BB_DELAY_US);
}

/* Returns the ACK bit sampled after the 8th clock: 0 = ACK, 1 = NAK. */
static uint8_t bbWriteByte(uint8_t sda, uint8_t scl, uint8_t v) {
  for (int i = 7; i >= 0; i--) {
    if ((v >> i) & 1) bbRelease(sda); else bbLow(sda);
    delayMicroseconds(BB_DELAY_US);
    bbRelease(scl);   delayMicroseconds(BB_DELAY_US);
    bbLow(scl);       delayMicroseconds(BB_DELAY_US);
  }
  bbRelease(sda);     delayMicroseconds(BB_DELAY_US);
  bbRelease(scl);     delayMicroseconds(BB_DELAY_US);
  uint8_t ack = digitalRead(sda);
  bbLow(scl);         delayMicroseconds(BB_DELAY_US);
  return ack;
}

static void bbScan(uint8_t sda, uint8_t scl) {
  /*
   * Bus recovery first. A device left mid-byte by an earlier malformed
   * transaction will hold SDA low forever, which makes every address appear to
   * ACK. Clocking SCL at least 9 times with SDA released lets it finish the
   * byte, then a STOP returns the bus to idle.
   */
  bbRelease(sda);
  for (int i = 0; i < 16; i++) {
    bbRelease(scl); delayMicroseconds(BB_DELAY_US);
    bbLow(scl);     delayMicroseconds(BB_DELAY_US);
  }
  bbRelease(scl);
  bbStop(sda, scl);
  delayMicroseconds(50);

  /* Idle levels: real I2C sits high on both via pull-ups. */
  bbRelease(sda); bbRelease(scl); delayMicroseconds(50);
  Serial.printf("idle: SDA(%u)=%d SCL(%u)=%d%s\r\n", sda, digitalRead(sda),
                scl, digitalRead(scl),
                (digitalRead(sda) && digitalRead(scl)) ? "  (looks like an idle I2C bus)"
                                                       : "  (NOT an idle I2C bus)");
  uint8_t found = 0, addrs[16];
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    bbStart(sda, scl);
    uint8_t ack = bbWriteByte(sda, scl, (uint8_t)(a << 1));
    bbStop(sda, scl);
    if (ack == 0) { if (found < 16) addrs[found] = a; found++; }
  }
  pinMode(sda, INPUT); pinMode(scl, INPUT);

  if (found > 12) {
    Serial.printf("  all %u addresses ACK -> bus is stuck low, no usable I2C here\r\n", found);
  } else if (!found) {
    Serial.println(F("  no devices"));
  } else {
    for (uint8_t i = 0; i < found; i++)
      Serial.printf("  device 0x%02X  (%s)\r\n", addrs[i], i2cGuess(addrs[i]));
  }
}

/*
 * Characterises every safe GPIO by reading it with the internal pull-up and
 * then the pull-down. This tells apart a floating pin from one the add-on
 * board actually drives or ties to a rail.
 */
static void probePins(void) {
  Serial.println(F("pin  pull-up  pull-down  interpretation"));
  for (size_t i = 0; i < SAFE_PIN_COUNT; i++) {
    uint8_t p = kSafePins[i];
    pinMode(p, INPUT_PULLUP);   delayMicroseconds(600);
    uint8_t up = digitalRead(p);
    pinMode(p, INPUT_PULLDOWN); delayMicroseconds(600);
    uint8_t dn = digitalRead(p);
    pinMode(p, INPUT);

    const char *what;
    if (up && !dn)      what = "floating (nothing attached)";
    else if (!up && !dn) what = "tied LOW  <-- driven or grounded";
    else if (up && dn)   what = "tied HIGH <-- driven or pulled up";
    else                 what = "inconsistent";
    Serial.printf("%-4u %-8u %-10u %s\r\n", p, up, dn, what);
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

  /*
   * Dumps the exact payloads the web panel consumes, so they can be inspected
   * and validated without a Wi-Fi client attached.
   */
  if (!strcmp(line, "json")) {
    String j;
    if (!strcmp(arg, "history")) {
      histJson(j, 0, 20, 0);
    } else {
      buildStatusJson(j);
    }
    Serial.println(F("---JSON-BEGIN---"));
    /* Chunked so a long document is not lost in the USB CDC TX buffer. */
    for (size_t i = 0; i < j.length(); i += 128) {
      Serial.print(j.substring(i, i + 128));
      Serial.flush();
      delay(4);
    }
    Serial.println();
    Serial.println(F("---JSON-END---"));
    Serial.printf("length: %u bytes\r\n", (unsigned)j.length());
    return;
  }

  /* Checks whether the peer's web panel is reachable over the HaLow link. */
  if (!strcmp(line, "httpget")) {
    IPAddress target;
    if (!target.fromString(arg[0] ? arg : IPAddress(g_cfg.peerIp).toString().c_str())) {
      Serial.println(F("usage: httpget <ip>"));
      return;
    }
    Serial.printf("GET http://%s/api/status ...\r\n", target.toString().c_str());
    WiFiClient c;
    c.setTimeout(8);
    uint32_t t0 = millis();
    if (!c.connect(target, 80, 8000)) {
      Serial.println(F("connect FAILED"));
      return;
    }
    c.printf("GET /api/status HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             target.toString().c_str());
    uint32_t bytes = 0;
    String firstLine;
    uint32_t deadline = millis() + 10000;
    while (c.connected() && millis() < deadline) {
      while (c.available()) {
        char ch = (char)c.read();
        bytes++;
        if (firstLine.length() < 40 && ch != '\r' && ch != '\n') firstLine += ch;
        deadline = millis() + 3000;
      }
      delay(10);
    }
    c.stop();
    Serial.printf("reply: \"%s\"  %lu bytes in %lu ms\r\n",
                  firstLine.c_str(), (unsigned long)bytes,
                  (unsigned long)(millis() - t0));
    return;
  }

  if (!strcmp(line, "fwd")) {
    if      (!strcmp(arg, "off") || !strcmp(arg, "isolated")) g_cfg.forwardMode = FWD_ISOLATED;
    else if (!strcmp(arg, "nat"))                             g_cfg.forwardMode = FWD_NAT;
    else if (!strcmp(arg, "route"))                           g_cfg.forwardMode = FWD_ROUTE;
    else { Serial.println(F("usage: fwd off|nat|route")); return; }
    Serial.printf("forwarding = %s (save + reboot to apply)\r\n",
                  forwardModeName(g_cfg.forwardMode));
    return;
  }

  if (!strcmp(line, "blink")) {
    /* Blinks the on-board LED so you can tell which physical node this is. */
    uint32_t secs = (uint32_t)atol(arg);
    if (secs == 0 || secs > 60) secs = 10;
    Serial.printf("blinking LED on GPIO%d for %lu s - watch the boards\r\n",
                  LED_BUILTIN, (unsigned long)secs);
    pinMode(LED_BUILTIN, OUTPUT);
    uint32_t end = millis() + secs * 1000;
    while (millis() < end) {
      digitalWrite(LED_BUILTIN, HIGH); delay(120);
      digitalWrite(LED_BUILTIN, LOW);  delay(120);
    }
    Serial.println(F("blink done"));
    return;
  }

  if (!strcmp(line, "i2cscan")) {
    int sda = -1, scl = -1;
    if (sscanf(arg, "%d %d", &sda, &scl) == 2) {
      if (!pinIsSafe((uint8_t)sda) || !pinIsSafe((uint8_t)scl)) {
        Serial.println(F("refusing: one of those pins is reserved (MM6108/USB/flash/UART)"));
        return;
      }
      Serial.println(F("scanning given pair..."));
      scanI2cPair((uint8_t)sda, (uint8_t)scl);
    } else {
      Serial.println(F("sweeping candidate SDA/SCL pairs..."));
      for (size_t i = 0; i < I2C_PAIR_COUNT; i++) {
        scanI2cPair(kI2cPairs[i][0], kI2cPairs[i][1]);
        delay(5);
      }
      Serial.println(F("done. usage for a specific pair: i2cscan <sda> <scl>"));
    }
    return;
  }

  if (!strcmp(line, "pins")) { probePins(); return; }

  if (!strcmp(line, "i2cbb")) {
    int sda = -1, scl = -1;
    if (sscanf(arg, "%d %d", &sda, &scl) != 2 ||
        !pinIsSafe((uint8_t)sda) || !pinIsSafe((uint8_t)scl)) {
      Serial.println(F("usage: i2cbb <sda> <scl>   (safe pins only)"));
      return;
    }
    Serial.printf("bit-banged I2C probe on SDA=%d SCL=%d\r\n", sda, scl);
    bbScan((uint8_t)sda, (uint8_t)scl);
    return;
  }

  if (!strcmp(line, "btnscan")) {
    /*
     * Samples every safe GPIO with an internal pull-up and reports the ones
     * that get pulled low - i.e. the buttons on the add-on board.
     */
    uint32_t secs = (uint32_t)atol(arg);
    if (secs == 0 || secs > 120) secs = 20;

    for (size_t i = 0; i < SAFE_PIN_COUNT; i++) pinMode(kSafePins[i], INPUT_PULLUP);
    delay(50);

    uint8_t idle[SAFE_PIN_COUNT];
    for (size_t i = 0; i < SAFE_PIN_COUNT; i++) idle[i] = digitalRead(kSafePins[i]);

    Serial.print(F("idle state: "));
    for (size_t i = 0; i < SAFE_PIN_COUNT; i++)
      if (!idle[i]) Serial.printf("GPIO%u=LOW ", kSafePins[i]);
    Serial.println(F("\r\npress each button now (one at a time)..."));

    uint32_t end = millis() + secs * 1000;
    while (millis() < end) {
      for (size_t i = 0; i < SAFE_PIN_COUNT; i++) {
        uint8_t v = digitalRead(kSafePins[i]);
        if (v != idle[i]) {
          Serial.printf("  GPIO%-2u -> %s\r\n", kSafePins[i], v ? "HIGH" : "LOW (pressed)");
          idle[i] = v;
        }
      }
      delay(15);
    }
    Serial.println(F("btnscan finished"));
    return;
  }

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
      /*
       * Not HaLow.HalowAPClass::isEnabled()/getChannel(): those track the
       * wrapper's own ap_args, which we bypass, so they read 0 and mislead.
       */
      ChannelInfo ci;
      if (halowChannelByNumber(g_cfg.region, g_cfg.channel, ci)) {
        Serial.printf("AP config        : chan %u  opclass %u  bw %u MHz  beacon %u TU\r\n",
                      ci.chanNum, (unsigned)ci.globalOpClass, ci.bwMhz,
                      halowBeaconIntervalTus());
      }
    } else {
      Serial.printf("mmwlan sta state : %d (0=DISABLED 1=CONNECTING 2=CONNECTED)\r\n",
                    (int)mmwlan_get_sta_state());
      Serial.printf("get_bssid        : %d\r\n", (int)mmwlan_get_bssid(b));
      Serial.printf("scan dwell       : %lu ms\r\n", (unsigned long)halowScanDwellMs());
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
