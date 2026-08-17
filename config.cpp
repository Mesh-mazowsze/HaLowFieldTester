#include "config.h"
#include "logbuf.h"

#include <Preferences.h>
#include <esp_mac.h>

AppConfig g_cfg;

static Preferences s_prefs;
static const char *NVS_NS  = "halowtest";
static const char *NVS_KEY = "cfg";

static uint32_t cfgCrc(const AppConfig &c) {
  /* FNV-1a over everything except the trailing crc field. */
  const uint8_t *p = (const uint8_t *)&c;
  size_t len = sizeof(AppConfig) - sizeof(uint32_t);
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

static uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t)IPAddress(a, b, c, d);
}

void cfgSetDefaults(AppConfig &c) {
  memset(&c, 0, sizeof(c));
  c.magic   = CFG_MAGIC;
  c.version = CFG_VERSION;
  c.size    = sizeof(AppConfig);

  c.role = ROLE_AP;

  strlcpy(c.halowSsid, "HaLow-Test", sizeof(c.halowSsid));
  strlcpy(c.halowPass, "halow12345", sizeof(c.halowPass));
  c.security = SEC_SAE;

  /*
   * EU / 868 MHz defaults for the HT-HC01 V2 low-band module.
   * Channel 5 = 865.5 MHz, 1 MHz bandwidth (see mmwlan_regdb.h, s1g_channels_EU).
   * Deliberately NOT a US/915 MHz default.
   */
  strlcpy(c.region, "EU", sizeof(c.region));
  c.channel    = 5;
  c.txPowerDbm = 0; /* use the regulatory maximum for the channel */

  c.netmask = ip4(255, 255, 255, 0);
  c.gateway = ip4(192, 168, 50, 1);
  /* Address depends on the role; fixed up in cfgLoad()/role change. */
  c.ip      = ip4(192, 168, 50, 1);
  c.peerIp  = ip4(192, 168, 50, 2);

  c.mgmtSsid[0] = '\0'; /* auto */
  strlcpy(c.mgmtPass, "halowtester", sizeof(c.mgmtPass));
  c.mgmtChannel = 6;

  c.iperfPort = 5001;
  c.rttPort   = 5555;
  c.peerPort  = 5556;

  c.contEnabled    = 0;
  c.contIntervalMs = 1000;

  c.crc = cfgCrc(c);
}

bool cfgLoad(void) {
  AppConfig tmp;
  bool ok = false;

  if (s_prefs.begin(NVS_NS, true /* read-only */)) {
    size_t len = s_prefs.getBytesLength(NVS_KEY);
    if (len == sizeof(AppConfig)) {
      s_prefs.getBytes(NVS_KEY, &tmp, sizeof(tmp));
      if (tmp.magic == CFG_MAGIC && tmp.version == CFG_VERSION &&
          tmp.size == sizeof(AppConfig) && tmp.crc == cfgCrc(tmp)) {
        g_cfg = tmp;
        ok = true;
      }
    }
    s_prefs.end();
  }

  if (!ok) {
    cfgSetDefaults(g_cfg);
    LOGW("CFG", "no valid stored config, using defaults (role=%s)", roleName(g_cfg.role));
  } else {
    LOGI("CFG", "loaded config: role=%s ssid=\"%s\" region=%s ch=%u",
         roleName(g_cfg.role), g_cfg.halowSsid, g_cfg.region, g_cfg.channel);
  }
  return ok;
}

bool cfgSave(void) {
  g_cfg.magic   = CFG_MAGIC;
  g_cfg.version = CFG_VERSION;
  g_cfg.size    = sizeof(AppConfig);
  g_cfg.crc     = cfgCrc(g_cfg);

  if (!s_prefs.begin(NVS_NS, false)) {
    LOGE("CFG", "NVS open for write failed");
    return false;
  }
  size_t n = s_prefs.putBytes(NVS_KEY, &g_cfg, sizeof(g_cfg));
  s_prefs.end();

  if (n != sizeof(g_cfg)) {
    LOGE("CFG", "NVS write short (%u/%u)", (unsigned)n, (unsigned)sizeof(g_cfg));
    return false;
  }
  LOGI("CFG", "configuration saved");
  return true;
}

void cfgFactoryReset(void) {
  if (s_prefs.begin(NVS_NS, false)) {
    s_prefs.clear();
    s_prefs.end();
  }
  cfgSetDefaults(g_cfg);
  LOGW("CFG", "factory reset performed");
}

const char *roleName(uint8_t role) {
  return (role == ROLE_STA) ? "STA" : "AP";
}

const char *securityName(uint8_t sec) {
  return (sec == SEC_OPEN) ? "OPEN" : "SAE";
}

String cfgHostname(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char buf[32];
  snprintf(buf, sizeof(buf), "halow-tester-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

String cfgMgmtSsid(void) {
  if (g_cfg.mgmtSsid[0] != '\0') {
    return String(g_cfg.mgmtSsid);
  }
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char buf[32];
  snprintf(buf, sizeof(buf), "HaLow-Tester-%02X%02X", mac[4], mac[5]);
  return String(buf);
}
