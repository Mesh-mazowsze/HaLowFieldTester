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
static bool      s_apEnabled     = false;
static uint16_t  s_beaconTus     = 0;   /* as programmed, AP role */
static uint32_t  s_scanDwellMs   = 0;   /* as programmed, STA role */

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
  /*
   * We program the AP channel ourselves (see startApDirect), and the STA joins
   * on the configured channel's regulatory entry, so the configured channel is
   * authoritative in both roles.
   */
  return halowChannelByNumber(g_cfg.region, g_cfg.channel, out);
}

/*
 * AP role: the Heltec wrapper's status is not used because we enable the AP
 * through the Morse API directly, so ask the driver instead.
 */
static bool linkIsUp(void) {
  if (g_cfg.role == ROLE_AP) {
    uint8_t bssid[6];
    return s_apEnabled && (mmwlan_ap_get_bssid(bssid) == MMWLAN_SUCCESS);
  }
  return HaLow.status() == WL_CONNECTED;
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

/*
 * Choose a beacon interval that keeps beaconing inside the channel's duty
 * cycle allowance.
 *
 * A beacon is sent at the lowest MCS. Rough airtime at MCS0, long GI:
 *   1 MHz -> 300 kbps  -> ~4-5 ms for a typical S1G beacon
 *   2 MHz -> 650 kbps  -> ~2 ms
 * The EU allows 2.80%. At the 100 TU (102.4 ms) default that puts 1 MHz
 * beaconing at ~4-5% - over the limit - so the driver stops sending them and
 * no STA can ever discover the AP. This is why Heltec's own AP example cannot
 * be joined on any EU 1 MHz channel, while 2 MHz works immediately.
 *
 * Verified on hardware: EU ch5 (1 MHz) never associates at 100 TU, and
 * associates in ~3 s at 300 TU.
 */
static uint16_t chooseBeaconInterval(const ChannelInfo &ci) {
  if (g_cfg.beaconIntervalTus > 0) {
    return g_cfg.beaconIntervalTus;   /* explicit override */
  }
  /* Unrestricted duty cycle (e.g. 100.00%): the default is fine. */
  if (ci.dutyCyclePct100 >= 10000) {
    return 100;
  }
  /* Duty-cycle limited. Narrow channels need a proportionally longer gap. */
  return (ci.bwMhz <= 1) ? 300 : 200;
}

static bool startApDirect(const ChannelInfo &ci,
                          enum mmwlan_security_type security,
                          const char *passphrase) {
  /*
   * Deliberately not using HaLow.AP(): that wrapper hardcodes
   * beacon_interval_tus = 0 (-> 100 TU default) and pri_bw_mhz = 0, which
   * cannot be configured through its API. We build the arguments ourselves and
   * call the Morse API directly so the beacon interval and primary bandwidth
   * are set explicitly.
   */
  const uint16_t beaconTus = chooseBeaconInterval(ci);

  halow_mode = MMWLAN_VIF_AP;

  struct mmwlan_ap_args args = MMWLAN_AP_ARGS_INIT;
  strncpy((char *)args.ssid, g_cfg.halowSsid, sizeof(args.ssid) - 1);
  args.ssid_len = strlen((const char *)args.ssid);
  strncpy(args.passphrase, passphrase, sizeof(args.passphrase) - 1);
  args.passphrase_len = strlen(args.passphrase);

  args.security_type       = security;
  args.pmf_mode            = MMWLAN_PMF_REQUIRED;
  args.op_class            = (uint16_t)ci.globalOpClass;
  args.s1g_chan_num        = ci.chanNum;
  args.pri_bw_mhz          = ci.bwMhz;   /* explicit, not "auto" */
  args.pri_1mhz_chan_idx   = 0;
  args.beacon_interval_tus = beaconTus;
  args.dtim_period         = 1;
  args.max_stas            = 4;

  mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);

  enum mmwlan_status st = mmwlan_ap_enable(&args);
  if (st != MMWLAN_SUCCESS) {
    LOGE("HALOW", "mmwlan_ap_enable() failed with status %d", (int)st);
    return false;
  }

  LOGI("HALOW", "AP up: op class %u, primary BW %u MHz, beacon %u TU (%lu ms)%s",
       args.op_class, args.pri_bw_mhz, beaconTus,
       (unsigned long)((beaconTus * 1024UL) / 1000UL),
       g_cfg.beaconIntervalTus ? " [manual]" : " [auto]");
  if (ci.dutyCyclePct100 < 10000 && ci.bwMhz <= 1) {
    LOGI("HALOW", "beacon interval stretched to stay within the %u.%02u%% duty "
                  "cycle limit for this channel",
         ci.dutyCyclePct100 / 100, ci.dutyCyclePct100 % 100);
  }
  s_apEnabled = true;
  s_beaconTus = beaconTus;
  return true;
}

uint16_t halowBeaconIntervalTus(void) { return s_beaconTus; }
uint32_t halowScanDwellMs(void)       { return s_scanDwellMs; }

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

  if (g_cfg.security == SEC_SAE && g_cfg.halowPass[0] == '\0') {
    LOGW("HALOW", "SAE selected but the passphrase is empty - falling back to OPEN; "
                  "the peer will not associate unless it is also open");
  }

  /*
   * Never pass NULL here. HalowClass::AP() guards against a NULL passphrase,
   * but HalowSTAClass::begin() feeds it straight into mmosal_safer_strcpy()
   * with no check (HalowSTA.cpp:147), which dereferences NULL and panics the
   * device into a boot loop. An empty string selects OPEN on both paths.
   */
  const char *pass = open ? "" : g_cfg.halowPass;

  if (g_cfg.role == ROLE_AP) {
    LOGI("HALOW", "starting AP \"%s\" (%s) on channel %u",
         g_cfg.halowSsid, open ? "OPEN" : "SAE", g_cfg.channel);
    if (!startApDirect(ci, open ? MMWLAN_OPEN : MMWLAN_SAE, pass)) {
      return false;
    }
  } else {
    /*
     * The driver's internal connect scan dwells for only
     * MMWLAN_SCAN_DEFAULT_DWELL_TIME_MS (30 ms) per channel. On a duty-cycle
     * limited 1 MHz channel the AP has to beacon slowly (see
     * chooseBeaconInterval), so a 30 ms dwell catches a beacon roughly one
     * time in ten and association becomes a coin flip - measured 1 in 3 even
     * with a working AP. Dwell for longer than one full beacon period.
     */
    struct mmwlan_scan_config sc = MMWLAN_SCAN_CONFIG_INIT;
    uint32_t beaconMs = ((uint32_t)chooseBeaconInterval(ci) * 1024UL) / 1000UL;
    sc.dwell_time_ms  = beaconMs + 120;
    if (mmwlan_set_scan_config(&sc) == MMWLAN_SUCCESS) {
      s_scanDwellMs = sc.dwell_time_ms;
      LOGI("HALOW", "scan dwell set to %lu ms (beacon period ~%lu ms)",
           (unsigned long)sc.dwell_time_ms, (unsigned long)beaconMs);
    } else {
      LOGW("HALOW", "mmwlan_set_scan_config() failed; association may be slow");
    }

    LOGI("HALOW", "starting STA, joining \"%s\" (%s)",
         g_cfg.halowSsid, open ? "OPEN" : "SAE");
    HaLow.begin(g_cfg.halowSsid, pass, open ? MMWLAN_OPEN : MMWLAN_SAE, g_cfg.region);
  }

  s_started = true;
  return true;
}

bool halowWaitForLink(uint32_t timeoutMs) {
  uint32_t start = millis();
  uint32_t lastDot = 0;

  LOGI("HALOW", "waiting up to %lu s for the HaLow link before starting the "
                "2.4 GHz management AP", (unsigned long)(timeoutMs / 1000));

  while ((uint32_t)(millis() - start) < timeoutMs) {
    halowTick();
    if (linkIsUp()) {
      LOGI("HALOW", "link established after %lu ms",
           (unsigned long)(millis() - start));
      return true;
    }
    if ((uint32_t)(millis() - lastDot) >= 5000) {
      lastDot = millis();
      LOGI("HALOW", "  ... still associating (%lu s)",
           (unsigned long)((millis() - start) / 1000));
    }
    delay(200);
  }

  LOGW("HALOW", "link did not come up within %lu s; starting the management AP "
                "anyway so the panel stays reachable",
       (unsigned long)(timeoutMs / 1000));
  return false;
}

void halowTick(void) {
  /*
   * The Arduino event for AP mode never reports GOT_IP the way STA does, so
   * track link state from the wrapper status as well.
   */
  bool up = linkIsUp();
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
