/*
 * throughput_test.h - TCP/UDP throughput using Morse Micro's own mmiperf.
 *
 * Design note: mmiperf exposes no stop/abort call - a session ends only when
 * its configured `amount` (bytes or time) is reached. So instead of starting
 * and stopping servers per test, each node brings up one TCP and one UDP
 * iperf server at link-up and leaves them running. A test then only ever
 * starts a *client*:
 *
 *   TX test: this node runs the client -> peer's server receives
 *   RX test: peer is told to run a client -> this node's server receives
 *
 * That makes every test naturally bounded and avoids leaking sockets.
 *
 * Not available from mmiperf, and therefore not reported:
 *   - TCP retransmission counts
 *   - a true jitter figure (the report carries inter-packet gap sums, which
 *     is reported as mean IPG, not relabelled as jitter)
 *   - simultaneous bidirectional tests (mmiperf has no dual mode)
 */
#pragma once

#include <Arduino.h>
#include "peer_link.h"

enum ThrDir : uint8_t {
  THR_DIR_TX = 0,   /* this node transmits */
  THR_DIR_RX = 1,   /* this node receives  */
};

enum ThrState : uint8_t {
  THR_IDLE     = 0,
  THR_ARMING   = 1,
  THR_RUNNING  = 2,
  THR_DONE     = 3,
  THR_FAILED   = 4,
};

struct ThroughputResult {
  uint8_t  state;
  uint8_t  dir;
  uint8_t  udp;
  uint32_t startedMs;
  uint32_t requestedS;

  uint32_t avgKbps;        /* final average */
  uint32_t curKbps;        /* live, from interim reports */
  uint64_t bytes;
  uint32_t durationMs;

  /* UDP only. */
  bool     udpStatsValid;
  uint32_t txFrames;
  uint32_t rxFrames;
  uint32_t outOfSeq;
  uint32_t errorCount;     /* lost/errored datagrams as counted by iperf */
  uint16_t lossPct100;
  bool     ipgValid;
  uint32_t meanIpgTenthMs; /* mean inter-packet gap; NOT jitter */

  char     localAddr[48];
  char     remoteAddr[48];
  uint16_t localPort;
  uint16_t remotePort;
  char     note[64];
};

void  thrInit(void);
void  thrTick(void);

/* Called once the HaLow link is up; idempotent. */
void  thrStartServers(void);

bool  thrStartTest(uint8_t dir, bool udp, uint16_t durationS,
                   uint32_t targetKbps, uint16_t packetSize, String &err);
void  thrAbort(void);   /* marks the local test finished; see note above */

const ThroughputResult &thrResult(void);
uint32_t thrLastKbps(void);   /* most recent throughput figure, 0 if none */

/* Hooks this module's command handler into peer_link (called from setup()). */
void  thrRegisterPeerHandler(void);
