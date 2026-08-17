/*
 * peer_link.h - telemetry and control exchange between the two tester nodes.
 *
 * Why this exists: RSSI and the rate-control table are STA-side measurements,
 * and the Heltec AP wrapper cannot enumerate associated stations
 * (HalowAPClass::getStationList() is a stub in the library). So each node
 * periodically sends its own measurements to the other over the HaLow link.
 * That is what lets you connect a phone to the AP node and still see the
 * remote node's signal quality.
 *
 * It also carries the command that arms the far end of a throughput test, so
 * one tap in the panel sets up both sides.
 *
 * Both ends run the same firmware on the same architecture, so the payload is
 * exchanged in native byte order.
 */
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#define PEER_FRESH_MS 5000

struct PeerInfo {
  bool      valid;
  uint32_t  lastSeenMs;
  IPAddress ip;

  uint8_t   role;
  bool      linkUp;
  bool      rssiValid;
  bool      rateValid;

  int16_t   rssiDbm;
  int8_t    mcs;
  uint8_t   bwMhz;
  bool      shortGi;
  uint32_t  phyRateKbps;
  uint16_t  perPct100;
  uint16_t  rttTenthMs;

  uint32_t  uptimeS;
  uint32_t  linkUptimeS;
  uint32_t  freeHeap;
  char      mac[18];
  char      fw[12];
};

/* Commands carried between nodes. */
enum PeerCmd : uint8_t {
  PEER_CMD_NONE          = 0,
  PEER_CMD_START_SERVER  = 1,  /* start an iperf server (peer will be the client) */
  PEER_CMD_START_CLIENT  = 2,  /* start an iperf client aimed back at us */
  PEER_CMD_STOP          = 3,
};

struct PeerCmdArgs {
  uint8_t  cmd;
  uint8_t  udp;         /* 0 = TCP, 1 = UDP */
  uint16_t port;
  uint16_t durationS;
  uint32_t targetKbps;  /* UDP only, 0 = unlimited */
  uint16_t packetSize;  /* UDP only, 0 = default */
  uint32_t targetIp;    /* who the client should connect to */
};

typedef void (*PeerCmdHandler)(const PeerCmdArgs &args);

void            peerInit(void);
void            peerTick(void);
const PeerInfo &peerData(void);
bool            peerIsFresh(void);

void            peerSetCommandHandler(PeerCmdHandler h);
bool            peerSendCommand(const PeerCmdArgs &args);
