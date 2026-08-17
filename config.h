/*
 * config.h - persistent configuration (NVS / Preferences) for the HaLow field tester.
 *
 * One firmware image serves both nodes; the role (HaLow AP or HaLow STA) is chosen
 * from the web panel and stored here, so it survives a reboot.
 */
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#define FW_VERSION      "1.0.0"
#define CFG_MAGIC       0x48544631UL /* 'HTF1' */
#define CFG_VERSION     1

enum Role : uint8_t {
  ROLE_AP  = 0,
  ROLE_STA = 1,
};

enum SecurityMode : uint8_t {
  SEC_OPEN = 0,
  SEC_SAE  = 1,
};

/*
 * Stored as a single blob in NVS. Keep it POD and versioned: a size/version
 * mismatch falls back to defaults rather than loading garbage.
 */
struct AppConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t size;

  /* --- role --- */
  uint8_t  role;

  /* --- HaLow radio --- */
  char     halowSsid[33];
  char     halowPass[65];
  uint8_t  security;        /* SecurityMode */
  char     region[4];       /* regulatory domain, e.g. "EU" */
  uint8_t  channel;         /* S1G channel number (EU 1 MHz: 1,3,5,7,9) */
  uint16_t txPowerDbm;      /* 0 = no override, use regulatory maximum */

  /* --- HaLow IP (no DHCP server on the HaLow AP, so both ends are static) --- */
  uint32_t ip;
  uint32_t netmask;
  uint32_t gateway;
  uint32_t peerIp;          /* the other node, used for RTT and throughput tests */

  /* --- 2.4 GHz management AP --- */
  char     mgmtSsid[33];    /* empty => auto "HaLow-Tester-XXXX" */
  char     mgmtPass[65];
  uint8_t  mgmtChannel;

  /* --- service ports --- */
  uint16_t iperfPort;
  uint16_t rttPort;
  uint16_t peerPort;

  /* --- continuous link test --- */
  uint8_t  contEnabled;
  uint16_t contIntervalMs;

  uint32_t crc;
};

extern AppConfig g_cfg;

void        cfgSetDefaults(AppConfig &c);
bool        cfgLoad(void);   /* true if a valid stored config was loaded */
bool        cfgSave(void);
void        cfgFactoryReset(void);

const char *roleName(uint8_t role);
const char *securityName(uint8_t sec);

/* Management SSID actually in use (resolves the "auto" case). */
String      cfgMgmtSsid(void);
String      cfgHostname(void);
