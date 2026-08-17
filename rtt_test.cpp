#include "rtt_test.h"
#include "config.h"
#include "logbuf.h"
#include "halow_manager.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>

#define RTT_MAGIC       0x48525454UL /* 'HRTT' */
#define RTT_TYPE_PROBE  0
#define RTT_TYPE_REPLY  1
#define RTT_WINDOW      64
#define RTT_TIMEOUT_MS  2000

struct __attribute__((packed)) RttPacket {
  uint32_t magic;
  uint8_t  type;
  uint8_t  pad[3];
  uint32_t seq;
  uint32_t txMs;      /* originator's millis(), echoed verbatim */
};

/* Outstanding probe tracking for loss accounting. */
struct ProbeSlot {
  uint32_t seq;
  uint32_t sentMs;
  bool     used;
  bool     acked;
  bool     counted;
};

static int        s_sock = -1;
static bool       s_probing = false;
static uint16_t   s_intervalMs = 1000;
static uint32_t   s_lastProbe = 0;
static uint32_t   s_seq = 0;
static ProbeSlot  s_slots[RTT_WINDOW];
static RttStats   s_stats;

/* Rolling accumulators over the last RTT_WINDOW completed probes. */
static uint32_t   s_rttSum = 0;
static uint32_t   s_rttSamples = 0;

static void resetAccumulators(void) {
  memset(s_slots, 0, sizeof(s_slots));
  memset(&s_stats, 0, sizeof(s_stats));
  s_stats.minTenthMs = 0xFFFF;
  s_rttSum = 0;
  s_rttSamples = 0;
}

void rttResetStats(void) {
  resetAccumulators();
  LOGI("RTT", "statistics reset");
}

void rttInit(void) {
  resetAccumulators();

  s_sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) {
    LOGE("RTT", "socket() failed");
    return;
  }

  int flags = lwip_fcntl(s_sock, F_GETFL, 0);
  lwip_fcntl(s_sock, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(g_cfg.rttPort);

  if (lwip_bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOGE("RTT", "bind to UDP/%u failed", g_cfg.rttPort);
    lwip_close(s_sock);
    s_sock = -1;
    return;
  }
  LOGI("RTT", "UDP echo responder listening on port %u", g_cfg.rttPort);
}

static ProbeSlot *slotFor(uint32_t seq) {
  return &s_slots[seq % RTT_WINDOW];
}

static void recordRtt(uint32_t rttMs) {
  uint16_t tenth = (uint16_t)((rttMs > 6553) ? 65530 : rttMs * 10);
  s_stats.lastTenthMs = tenth;
  s_stats.valid = true;
  if (tenth < s_stats.minTenthMs) s_stats.minTenthMs = tenth;
  if (tenth > s_stats.maxTenthMs) s_stats.maxTenthMs = tenth;

  s_rttSum += tenth;
  s_rttSamples++;
  s_stats.avgTenthMs = (uint16_t)(s_rttSum / s_rttSamples);

  /* Keep the average responsive rather than lifetime-cumulative. */
  if (s_rttSamples >= 256) {
    s_rttSum     = s_stats.avgTenthMs * 32;
    s_rttSamples = 32;
  }
}

static void updateLoss(void) {
  uint32_t now = millis();
  for (int i = 0; i < RTT_WINDOW; i++) {
    ProbeSlot *p = &s_slots[i];
    if (p->used && !p->acked && !p->counted &&
        (uint32_t)(now - p->sentMs) > RTT_TIMEOUT_MS) {
      p->counted = true;
      s_stats.lost++;
    }
  }
  uint32_t total = s_stats.received + s_stats.lost;
  s_stats.lossPct100 = total ? (uint16_t)((uint64_t)s_stats.lost * 10000ULL / total)
                             : SAMPLE_NA_U16;
}

static void handleRx(void) {
  if (s_sock < 0) return;

  RttPacket pkt;
  struct sockaddr_in from;
  socklen_t fromLen;

  for (int guard = 0; guard < 16; guard++) {
    fromLen = sizeof(from);
    int n = lwip_recvfrom(s_sock, &pkt, sizeof(pkt), 0,
                          (struct sockaddr *)&from, &fromLen);
    if (n < 0) break;                       /* nothing left to read */
    if (n != (int)sizeof(pkt)) continue;    /* stray packet: skip, keep draining */
    if (ntohl(pkt.magic) != RTT_MAGIC) continue;

    if (pkt.type == RTT_TYPE_PROBE) {
      /* Echo it straight back, unmodified except for the type. */
      pkt.type = RTT_TYPE_REPLY;
      lwip_sendto(s_sock, &pkt, sizeof(pkt), 0,
                  (struct sockaddr *)&from, fromLen);
    } else if (pkt.type == RTT_TYPE_REPLY) {
      uint32_t seq = ntohl(pkt.seq);
      ProbeSlot *p = slotFor(seq);
      if (p->used && p->seq == seq && !p->acked) {
        p->acked = true;
        s_stats.received++;
        recordRtt(millis() - ntohl(pkt.txMs));
      }
    }
  }
}

static void sendProbe(void) {
  if (s_sock < 0) return;
  if (g_cfg.peerIp == 0) return;

  uint32_t seq   = s_seq + 1;
  uint32_t nowMs = millis();

  RttPacket pkt;
  pkt.magic = htonl(RTT_MAGIC);
  pkt.type  = RTT_TYPE_PROBE;
  pkt.pad[0] = pkt.pad[1] = pkt.pad[2] = 0;
  pkt.seq   = htonl(seq);
  pkt.txMs  = htonl(nowMs);

  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family      = AF_INET;
  to.sin_addr.s_addr = g_cfg.peerIp;   /* IPAddress stores network byte order */
  to.sin_port        = htons(g_cfg.rttPort);

  if (lwip_sendto(s_sock, &pkt, sizeof(pkt), 0,
                  (struct sockaddr *)&to, sizeof(to)) <= 0) {
    /*
     * Nothing left the box, so this is not a lost probe - arming the slot here
     * would inflate the loss figure whenever the link is simply down.
     */
    return;
  }

  s_seq = seq;
  ProbeSlot *p = slotFor(seq);
  p->seq     = seq;
  p->sentMs  = nowMs;
  p->used    = true;
  p->acked   = false;
  p->counted = false;
  s_stats.sent++;
}

void rttTick(void) {
  handleRx();

  if (s_probing && halowIsUp()) {
    uint32_t now = millis();
    if ((uint32_t)(now - s_lastProbe) >= s_intervalMs) {
      s_lastProbe = now;
      sendProbe();
    }
  }
  updateLoss();
}

void rttSetProbing(bool enable, uint16_t intervalMs) {
  if (intervalMs < 100) intervalMs = 100;
  s_intervalMs = intervalMs;
  if (enable != s_probing) {
    s_probing = enable;
    LOGI("RTT", "continuous probing %s (%u ms to %s)",
         enable ? "enabled" : "disabled", intervalMs,
         IPAddress(g_cfg.peerIp).toString().c_str());
  }
}

bool rttProbing(void) {
  return s_probing;
}

const RttStats &rttStats(void) {
  return s_stats;
}

uint16_t rttLastTenthMs(void) {
  return s_stats.valid ? s_stats.lastTenthMs : SAMPLE_NA_U16;
}

uint16_t rttLossPct100(void) {
  return s_stats.lossPct100;
}
