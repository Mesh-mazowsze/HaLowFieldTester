/*
 * halow_manager.h - brings the MM6108 / HT-HC01 V2 up in the configured role
 * and exposes the radio identity + regulatory information the panel shows.
 *
 * Everything here comes from the Morse Micro API (mmwlan.h) or the Heltec
 * wrapper; nothing is synthesised.
 */
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

/* Versions / identity read from the transceiver and the linked BCF. */
struct RadioInfo {
  bool   valid;              /* mmwlan_get_version() succeeded */
  char   morselibVersion[40];
  char   morseFwVersion[40];
  char   chipIdString[40];
  uint32_t chipId;

  bool   bcfValid;           /* mmwlan_get_bcf_metadata() succeeded */
  uint16_t bcfMajor;
  uint8_t  bcfMinor;
  uint8_t  bcfPatch;
  char   bcfBoardDesc[36];
  char   bcfBuildVersion[36];

  char   sdkVersion[24];     /* MM_VERSION_BUILDID, compile-time */
};

/* One entry of the active regulatory domain's channel list. */
struct ChannelInfo {
  uint8_t  chanNum;
  uint32_t centreFreqHz;
  uint8_t  bwMhz;
  int8_t   maxTxEirpDbm;
  uint16_t dutyCyclePct100;  /* hundredths of a percent */
  int16_t  globalOpClass;
  int16_t  s1gOpClass;
};

void        halowInit(void);          /* set up event hooks, power the module */
bool        halowStart(void);         /* apply config and start AP or STA */
void        halowTick(void);          /* housekeeping, called from loop() */

bool        halowIsUp(void);          /* link established (AP running / STA associated) */
uint32_t    halowLinkUptimeMs(void);  /* 0 when down */
uint32_t    halowDisconnectCount(void);

const RadioInfo &halowRadioInfo(void);

/* Regulatory helpers, read from the library's own regulatory database. */
uint8_t     halowChannelCount(const char *region);
bool        halowChannelAt(const char *region, uint8_t index, ChannelInfo &out);
bool        halowChannelByNumber(const char *region, uint8_t chanNum, ChannelInfo &out);
bool        halowRegionExists(const char *region);
uint8_t     halowRegionCount(void);
const char *halowRegionName(uint8_t index);

/* The channel actually in use, best-effort. */
bool        halowCurrentChannel(ChannelInfo &out);

IPAddress   halowLocalIP(void);
String      halowPeerMac(void);   /* BSSID: the AP we are joined to, or our own BSSID in AP mode */
String      halowOwnMac(void);
