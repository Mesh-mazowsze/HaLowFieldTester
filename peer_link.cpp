#include "peer_link.h"
#include "config.h"
#include "logbuf.h"
#include "link_monitor.h"
#include "halow_manager.h"
#include "rtt_test.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>

#define PEER_MAGIC     0x48505452UL /* 'HPTR' */
#define PEER_VERSION   1
#define PEER_TX_PERIOD 1000

#define MSG_TELEMETRY  0
#define MSG_COMMAND    1

struct __attribute__((packed)) PeerMsgHeader {
  uint32_t magic;
  uint8_t  version;
  uint8_t  type;
};

struct __attribute__((packed)) PeerTelemetryMsg {
  PeerMsgHeader hdr;
  uint8_t  role;
  uint8_t  flags;        /* bit0 linkUp, bit1 rssiValid, bit2 rateValid, bit3 sgi */
  int16_t  rssiDbm;
  int8_t   mcs;
  uint8_t  bwMhz;
  uint16_t perPct100;
  uint32_t phyRateKbps;
  uint16_t rttTenthMs;
  uint32_t uptimeS;
  uint32_t linkUptimeS;
  uint32_t freeHeap;
  char     mac[18];
  char     fw[12];
};

struct __attribute__((packed)) PeerCommandMsg {
  PeerMsgHeader hdr;
  PeerCmdArgs   args;
};

#define PF_LINK_UP    0x01
#define PF_RSSI_VALID 0x02
#define PF_RATE_VALID 0x04
#define PF_SGI        0x08

static int            s_sock = -1;
static PeerInfo       s_peer;
static uint32_t       s_lastTx = 0;
static PeerCmdHandler s_handler = NULL;

void peerInit(void) {
  s_peer = PeerInfo();   /* PeerInfo holds an IPAddress, so value-initialise */

  s_sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) {
    LOGE("PEER", "socket() failed");
    return;
  }
  int flags = lwip_fcntl(s_sock, F_GETFL, 0);
  lwip_fcntl(s_sock, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(g_cfg.peerPort);

  if (lwip_bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOGE("PEER", "bind to UDP/%u failed", g_cfg.peerPort);
    lwip_close(s_sock);
    s_sock = -1;
    return;
  }
  LOGI("PEER", "telemetry link listening on UDP/%u", g_cfg.peerPort);
}

void peerSetCommandHandler(PeerCmdHandler h) {
  s_handler = h;
}

static void sendTelemetry(void) {
  if (s_sock < 0 || g_cfg.peerIp == 0) return;

  const LinkStats &ls = linkStats();

  PeerTelemetryMsg m;
  memset(&m, 0, sizeof(m));
  m.hdr.magic   = PEER_MAGIC;
  m.hdr.version = PEER_VERSION;
  m.hdr.type    = MSG_TELEMETRY;

  m.role  = g_cfg.role;
  m.flags = 0;
  if (ls.linkUp)    m.flags |= PF_LINK_UP;
  if (ls.rssiValid) m.flags |= PF_RSSI_VALID;
  if (ls.rateValid) m.flags |= PF_RATE_VALID;
  if (ls.shortGi)   m.flags |= PF_SGI;

  m.rssiDbm     = ls.rssiValid ? ls.rssiDbm : 0;
  m.mcs         = ls.rateValid ? ls.mcs : -1;
  m.bwMhz       = ls.bwMhz;
  m.perPct100   = ls.perValid ? ls.perPct100 : 0xFFFF;
  m.phyRateKbps = ls.phyRateKbps;
  m.rttTenthMs  = rttLastTenthMs();
  m.uptimeS     = millis() / 1000;
  m.linkUptimeS = ls.linkUptimeMs / 1000;
  m.freeHeap    = ESP.getFreeHeap();
  strlcpy(m.mac, halowOwnMac().c_str(), sizeof(m.mac));
  strlcpy(m.fw,  FW_VERSION, sizeof(m.fw));

  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family      = AF_INET;
  to.sin_addr.s_addr = g_cfg.peerIp;
  to.sin_port        = htons(g_cfg.peerPort);

  lwip_sendto(s_sock, &m, sizeof(m), 0, (struct sockaddr *)&to, sizeof(to));
}

static void handleTelemetry(const PeerTelemetryMsg *m, const struct sockaddr_in *from) {
  s_peer.valid       = true;
  s_peer.lastSeenMs  = millis();
  s_peer.ip          = IPAddress(from->sin_addr.s_addr);
  s_peer.role        = m->role;
  s_peer.linkUp      = (m->flags & PF_LINK_UP)    != 0;
  s_peer.rssiValid   = (m->flags & PF_RSSI_VALID) != 0;
  s_peer.rateValid   = (m->flags & PF_RATE_VALID) != 0;
  s_peer.shortGi     = (m->flags & PF_SGI)        != 0;
  s_peer.rssiDbm     = m->rssiDbm;
  s_peer.mcs         = m->mcs;
  s_peer.bwMhz       = m->bwMhz;
  s_peer.perPct100   = m->perPct100;
  s_peer.phyRateKbps = m->phyRateKbps;
  s_peer.rttTenthMs  = m->rttTenthMs;
  s_peer.uptimeS     = m->uptimeS;
  s_peer.linkUptimeS = m->linkUptimeS;
  s_peer.freeHeap    = m->freeHeap;
  strlcpy(s_peer.mac, m->mac, sizeof(s_peer.mac));
  strlcpy(s_peer.fw,  m->fw,  sizeof(s_peer.fw));
}

static void handleRx(void) {
  if (s_sock < 0) return;

  uint8_t buf[sizeof(PeerTelemetryMsg) > sizeof(PeerCommandMsg)
                ? sizeof(PeerTelemetryMsg) : sizeof(PeerCommandMsg)];
  struct sockaddr_in from;
  socklen_t fromLen;

  for (int guard = 0; guard < 8; guard++) {
    fromLen = sizeof(from);
    int n = lwip_recvfrom(s_sock, buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromLen);
    if (n < 0) break;   /* nothing left to read */
    /* A stray datagram of the wrong size must not stop the drain loop. */
    if (n < (int)sizeof(PeerMsgHeader)) continue;

    const PeerMsgHeader *h = (const PeerMsgHeader *)buf;
    if (h->magic != PEER_MAGIC || h->version != PEER_VERSION) continue;

    if (h->type == MSG_TELEMETRY && n == (int)sizeof(PeerTelemetryMsg)) {
      handleTelemetry((const PeerTelemetryMsg *)buf, &from);
    } else if (h->type == MSG_COMMAND && n == (int)sizeof(PeerCommandMsg)) {
      /*
       * PeerCmdArgs is not packed, but inside the packed message it starts at
       * offset 6, so its 32-bit members land on 2-mod-4 addresses. Binding a
       * PeerCmdArgs& straight into the buffer would let the compiler emit
       * aligned loads and fault on Xtensa. Copy it out into an aligned local.
       */
      PeerCmdArgs a;
      memcpy(&a, buf + sizeof(PeerMsgHeader), sizeof(a));
      LOGI("PEER", "received test command %u from %s", a.cmd,
           IPAddress(from.sin_addr.s_addr).toString().c_str());
      if (s_handler) s_handler(a);
    }
  }
}

bool peerSendCommand(const PeerCmdArgs &args) {
  if (s_sock < 0 || g_cfg.peerIp == 0) return false;

  PeerCommandMsg m;
  memset(&m, 0, sizeof(m));
  m.hdr.magic   = PEER_MAGIC;
  m.hdr.version = PEER_VERSION;
  m.hdr.type    = MSG_COMMAND;
  m.args        = args;

  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family      = AF_INET;
  to.sin_addr.s_addr = g_cfg.peerIp;
  to.sin_port        = htons(g_cfg.peerPort);

  /*
   * UDP, so send a few copies to survive a lost packet. Sent back-to-back
   * rather than with a delay between them: this runs inside the HTTP handler,
   * and blocking here would stall the SSE stream and link sampling too.
   */
  bool ok = false;
  for (int i = 0; i < 3; i++) {
    if (lwip_sendto(s_sock, &m, sizeof(m), 0,
                    (struct sockaddr *)&to, sizeof(to)) > 0) {
      ok = true;
    }
  }
  if (ok) {
    LOGI("PEER", "sent test command %u to %s", args.cmd,
         IPAddress(g_cfg.peerIp).toString().c_str());
  } else {
    LOGW("PEER", "failed to send test command %u", args.cmd);
  }
  return ok;
}

void peerTick(void) {
  handleRx();

  uint32_t now = millis();
  if ((uint32_t)(now - s_lastTx) >= PEER_TX_PERIOD) {
    s_lastTx = now;
    sendTelemetry();
  }
}

const PeerInfo &peerData(void) {
  return s_peer;
}

bool peerIsFresh(void) {
  return s_peer.valid && (uint32_t)(millis() - s_peer.lastSeenMs) < PEER_FRESH_MS;
}
