#include "halow_manager.h"
#include "config.h"
#include "logbuf.h"

#include <HaLow.h>
#include "mmwlan.h"
#include "mmwlan_regdb.h"   /* regulatory database; const tables have internal linkage */
#include "mmversion.h"

static RadioInfo s_radio;
static bool      s_linkUp        = false;
static uint32_t  s_linkUpSince   = 0;
static uint32_t  s_disconnects   = 0;
static bool      s_started       = false;

/* ------------------------------------------------------------------ */
/* Regulatory database access                                          */
/* ------------------------------------------------------------------ */

static const struct mmwlan_s1g_channel_list *findDomain(const char *region) {
  const struct mmwlan_regulatory_db *db = get_regulatory_db();
  if (!db || !region) return NULL;
  for (unsigned i = 0; i < db->num_domains; i++) {
    const struct mmwlan_s1g_channel_list *d = db->domains[i];
    if (d && strncmp((const char *)d->country_code, region, 2) == 0) {
      return d;
    }
  }
  return NULL;
}

uint8_t halowRegionCount(void) {
  const struct mmwlan_regulatory_db *db = get_regulatory_db();
  return db ? (uint8_t)db->num_domains : 0;
}

const char *halowRegionName(uint8_t index) {
  const struct mmwlan_regulatory_db *db = get_regulatory_db();
  if (!db || index >= db->num_domains) return "";
  return (const char *)db->domains[index]->country_code;
}

bool halowRegionExists(const char *region) {
  return findDomain(region) != NULL;
}

uint8_t halowChannelCount(const char *region) {
  const struct mmwlan_s1g_channel_list *d = findDomain(region);
  return d ? (uint8_t)d->num_channels : 0;
}

static void fillChannelInfo(const struct mmwlan_s1g_channel *c, ChannelInfo &out) {
  out.chanNum         = c->s1g_chan_num;
  out.centreFreqHz    = c->centre_freq_hz;
  out.bwMhz           = c->bw_mhz;
  out.maxTxEirpDbm    = c->max_tx_eirp_dbm;
  out.dutyCyclePct100 = c->duty_cycle_sta;
  out.globalOpClass   = c->global_operating_class;
  out.s1gOpClass      = c->s1g_operating_class;
}

bool halowChannelAt(const char *region, uint8_t index, ChannelInfo &out) {
  const struct mmwlan_s1g_channel_list *d = findDomain(region);
  if (!d || index >= d->num_channels) return false;
  fillChannelInfo(&d->channels[index], out);
  return true;
}

bool halowChannelByNumber(const char *region, uint8_t chanNum, ChannelInfo &out) {
  const struct mmwlan_s1g_channel_list *d = findDomain(region);
  if (!d) return false;
  for (unsigned i = 0; i < d->num_channels; i++) {
    if (d->channels[i].s1g_chan_num == chanNum) {
      fillChannelInfo(&d->channels[i], out);
      return true;
    }
  }
  return false;
}

bool halowCurrentChannel(ChannelInfo &out) {
  uint8_t ch = 0;

  if (g_cfg.role == ROLE_AP) {
    /* AP mode: the wrapper keeps the channel it was started on. */
    ch = (uint8_t)HaLow.HalowAPClass::getChannel();
  }
  if (ch == 0) {
    /* STA mode (or AP not yet up): fall back to the configured channel. */
    ch = g_cfg.channel;
  }
  return halowChannelByNumber(g_cfg.region, ch, out);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void onHalowEvent(HaLowEvent_t event) {
  switch (event) {
    case ARDUINO_HALOW_EVENT_READY:
      LOGI("HALOW", "subsystem ready");
      break;
    case ARDUINO_HALOW_EVENT_STA_CONNECTING:
      LOGI("HALOW", "STA connecting to \"%s\"", g_cfg.halowSsid);
      break;
    case ARDUINO_HALOW_EVENT_STA_CONNECTED:
      LOGI("HALOW", "STA associated");
      break;
    case ARDUINO_HALOW_EVENT_STA_DISCONNECTED:
      if (s_linkUp) s_disconnects++;
      s_linkUp = false;
      s_linkUpSince = 0;
      /* The Morse API exposes no disconnect reason code, so none is reported. */
      LOGW("HALOW", "STA disconnected (total disconnects: %lu)",
           (unsigned long)s_disconnects);
      break;
    case ARDUINO_HALOW_EVENT_GOT_IP: {
      IPAddress ip = HaLow.localIP();
      s_linkUp = true;
      s_linkUpSince = millis();
      LOGI("HALOW", "link up, IP %s gw %s", ip.toString().c_str(),
           HaLow.gatewayIP().toString().c_str());
      break;
    }
    case ARDUINO_HALOW_EVENT_LOST_IP:
      LOGW("HALOW", "lost IP");
      s_linkUp = false;
      s_linkUpSince = 0;
      break;
    case ARDUINO_HALOW_EVENT_STA_START:
      LOGI("HALOW", "STA started");
      break;
    case ARDUINO_HALOW_EVENT_STA_STOP:
      LOGI("HALOW", "STA stopped");
      break;
    case ARDUINO_HALOW_EVENT_SCAN_DONE:
      LOGD("HALOW", "scan done");
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------ */
/* Version / identity                                                  */
/* ------------------------------------------------------------------ */

static void readRadioInfo(void) {
  memset(&s_radio, 0, sizeof(s_radio));
  strlcpy(s_radio.sdkVersion, MM_VERSION_BUILDID, sizeof(s_radio.sdkVersion));

  struct mmwlan_version v;
  memset(&v, 0, sizeof(v));
  if (mmwlan_get_version(&v) == MMWLAN_SUCCESS) {
    s_radio.valid = true;
    strlcpy(s_radio.morselibVersion, v.morselib_version, sizeof(s_radio.morselibVersion));
    strlcpy(s_radio.morseFwVersion,  v.morse_fw_version,  sizeof(s_radio.morseFwVersion));
    strlcpy(s_radio.chipIdString,    v.morse_chip_id_string, sizeof(s_radio.chipIdString));
    s_radio.chipId = v.morse_chip_id;
    LOGI("RADIO", "morselib %s, MM fw %s, chip %s (0x%08lX)",
         s_radio.morselibVersion, s_radio.morseFwVersion,
         s_radio.chipIdString, (unsigned long)s_radio.chipId);
  } else {
    LOGW("RADIO", "mmwlan_get_version() failed");
  }

  struct mmwlan_bcf_metadata bcf;
  memset(&bcf, 0, sizeof(bcf));
  if (mmwlan_get_bcf_metadata(&bcf) == MMWLAN_SUCCESS) {
    s_radio.bcfValid = true;
    s_radio.bcfMajor = bcf.version.major;
    s_radio.bcfMinor = bcf.version.minor;
    s_radio.bcfPatch = bcf.version.patch;
    strlcpy(s_radio.bcfBoardDesc,    bcf.board_desc,    sizeof(s_radio.bcfBoardDesc));
    strlcpy(s_radio.bcfBuildVersion, bcf.build_version, sizeof(s_radio.bcfBuildVersion));
    LOGI("RADIO", "BCF v%u.%u.%u \"%s\" build %s", (unsigned)bcf.version.major,
         (unsigned)bcf.version.minor, (unsigned)bcf.version.patch,
         s_radio.bcfBoardDesc, s_radio.bcfBuildVersion);
  } else {
    LOGW("RADIO", "mmwlan_get_bcf_metadata() failed");
  }
}

const RadioInfo &halowRadioInfo(void) {
  return s_radio;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

void halowInit(void) {
#ifdef HT_RC3268
  /* The HT-HC01 V2 module is fed by a switchable LDO on the RC3268. */
  pinMode(HALOW_LDO_CTRL, OUTPUT);
  digitalWrite(HALOW_LDO_CTRL, HALOW_LDO_ENABLE);
  LOGI("HALOW", "HC01 LDO enabled (GPIO%d)", HALOW_LDO_CTRL);
  delay(50);
#else
  LOGW("HALOW", "HT_RC3268 not defined - LDO control skipped");
#endif

  HaLow.onEvent(onHalowEvent);
}

bool halowStart(void) {
  if (s_started) {
    LOGW("HALOW", "already started");
    return true;
  }

  if (!halowRegionExists(g_cfg.region)) {
    LOGE("HALOW", "region \"%s\" is not in the regulatory database", g_cfg.region);
    return false;
  }

  ChannelInfo ci;
  if (!halowChannelByNumber(g_cfg.region, g_cfg.channel, ci)) {
    LOGE("HALOW", "channel %u is not valid for region %s", g_cfg.channel, g_cfg.region);
    return false;
  }
  LOGI("HALOW", "region %s channel %u = %lu.%lu MHz, %u MHz BW, regulatory max %d dBm EIRP",
       g_cfg.region, ci.chanNum,
       (unsigned long)(ci.centreFreqHz / 1000000UL),
       (unsigned long)((ci.centreFreqHz % 1000000UL) / 100000UL),
       ci.bwMhz, (int)ci.maxTxEirpDbm);

  /* Static addressing: the HaLow AP in this framework has no DHCP server. */
  IPAddress ip(g_cfg.ip), gw(g_cfg.gateway), mask(g_cfg.netmask);
  if (!HaLow.config(ip, gw, mask)) {
    LOGE("HALOW", "HaLow.config() rejected %s/%s gw %s", ip.toString().c_str(),
         mask.toString().c_str(), gw.toString().c_str());
    return false;
  }
  LOGI("HALOW", "static IP %s mask %s gw %s", ip.toString().c_str(),
       mask.toString().c_str(), gw.toString().c_str());

  HaLow.init(g_cfg.region);
  LOGI("HALOW", "initialised for region %s", g_cfg.region);

  readRadioInfo();

  /*
   * TX power override. The Morse API can only *lower* the regulatory maximum,
   * never raise it (see mmwlan_override_max_tx_power docs).
   */
  if (g_cfg.txPowerDbm > 0) {
    enum mmwlan_status st = mmwlan_override_max_tx_power(g_cfg.txPowerDbm);
    if (st == MMWLAN_SUCCESS) {
      LOGI("HALOW", "max TX power override set to %u dBm", g_cfg.txPowerDbm);
    } else {
      LOGW("HALOW", "TX power override to %u dBm failed (status %d)",
           g_cfg.txPowerDbm, (int)st);
    }
  }

  const bool open = (g_cfg.security == SEC_OPEN) || (g_cfg.halowPass[0] == '\0');

  if (g_cfg.role == ROLE_AP) {
    LOGI("HALOW", "starting AP \"%s\" (%s) on channel %u",
         g_cfg.halowSsid, open ? "OPEN" : "SAE", g_cfg.channel);
    if (!HaLow.AP(g_cfg.halowSsid, open ? NULL : g_cfg.halowPass, g_cfg.channel)) {
      LOGE("HALOW", "HaLow.AP() failed");
      return false;
    }
  } else {
    LOGI("HALOW", "starting STA, joining \"%s\" (%s)",
         g_cfg.halowSsid, open ? "OPEN" : "SAE");
    HaLow.begin(g_cfg.halowSsid, open ? NULL : g_cfg.halowPass,
                open ? MMWLAN_OPEN : MMWLAN_SAE, g_cfg.region);
  }

  s_started = true;
  return true;
}

void halowTick(void) {
  /*
   * The Arduino event for AP mode never reports GOT_IP the way STA does, so
   * track link state from the wrapper status as well.
   */
  bool up = (HaLow.status() == WL_CONNECTED);
  if (up != s_linkUp) {
    if (up) {
      s_linkUp = true;
      s_linkUpSince = millis();
      LOGI("HALOW", "%s link is up", roleName(g_cfg.role));
      /*
       * mmwlan_get_version() returns an empty firmware string until the
       * transceiver has been powered on at least once, so re-read it here if
       * the attempt during start-up came back blank.
       */
      if (!s_radio.valid || s_radio.morseFwVersion[0] == '\0') {
        readRadioInfo();
      }
    } else {
      s_disconnects++;
      s_linkUp = false;
      s_linkUpSince = 0;
      LOGW("HALOW", "%s link is down", roleName(g_cfg.role));
    }
  }
}

bool halowIsUp(void) {
  return s_linkUp;
}

uint32_t halowLinkUptimeMs(void) {
  return s_linkUp && s_linkUpSince ? (millis() - s_linkUpSince) : 0;
}

uint32_t halowDisconnectCount(void) {
  return s_disconnects;
}

IPAddress halowLocalIP(void) {
  return (g_cfg.role == ROLE_AP) ? IPAddress(g_cfg.ip) : HaLow.localIP();
}

static String macToStr(const uint8_t *m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

String halowOwnMac(void) {
  uint8_t mac[6] = {0};
  enum mmwlan_vif vif = (g_cfg.role == ROLE_AP) ? MMWLAN_VIF_AP : MMWLAN_VIF_STA;
  if (mmwlan_get_vif_mac_addr(vif, mac) == MMWLAN_SUCCESS) {
    return macToStr(mac);
  }
  return String();
}

String halowPeerMac(void) {
  uint8_t bssid[6] = {0};
  if (g_cfg.role == ROLE_STA) {
    /* BSSID of the AP we are associated with. */
    if (mmwlan_get_bssid(bssid) == MMWLAN_SUCCESS) {
      return macToStr(bssid);
    }
  } else {
    /*
     * In AP mode the Morse API can look up a station by MAC, but the Heltec
     * wrapper's getStationList() is a stub, so there is no way to enumerate
     * associated stations. The connected STA reports its own MAC to us over
     * the peer telemetry link instead - see peer_link.cpp.
     */
  }
  return String();
}
