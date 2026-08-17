#include "throughput_test.h"
#include "config.h"
#include "logbuf.h"
#include "halow_manager.h"
#include "stats.h"      /* SAMPLE_NA_U16 */

#include "mmiperf.h"

#define THR_POLL_MS      500
#define THR_ARM_DELAY_MS 400
#define THR_GRACE_MS     8000   /* how long past the requested duration we wait */

static ThroughputResult s_res;
static mmiperf_handle_t s_tcpServer = NULL;
static mmiperf_handle_t s_udpServer = NULL;
static mmiperf_handle_t s_client    = NULL;
static uint32_t         s_lastPoll  = 0;
static uint32_t         s_lastKbps  = 0;

/*
 * The session whose report we are actually waiting for. mmiperf hands the
 * generating handle to the callback, so we can ignore reports from sessions we
 * are no longer tracking - e.g. a test the user stopped, whose mmiperf session
 * keeps running and would otherwise terminate the *next* test with stale
 * numbers.
 */
static mmiperf_handle_t s_tracked = NULL;

/*
 * The report callback runs in the mmiperf task, so the handoff to loop() is
 * guarded by a spinlock rather than relying on `volatile` ordering (the
 * compiler is free to move a non-volatile memcpy across a volatile store).
 */
static bool                    s_reportPending = false;
static struct mmiperf_report   s_report;
static portMUX_TYPE            s_reportMux = portMUX_INITIALIZER_UNLOCKED;

/* Pending client start, so we can delay it until the peer has been armed. */
static bool     s_pendingClient = false;
static uint32_t s_pendingAtMs   = 0;
static struct mmiperf_client_args s_pendingArgs;
static bool     s_pendingUdp    = false;

static const char *reportTypeName(uint8_t t) {
  switch (t) {
    case MMIPERF_TCP_DONE_SERVER: return "TCP server done";
    case MMIPERF_TCP_DONE_CLIENT: return "TCP client done";
    case MMIPERF_TCP_ABORTED_LOCAL: return "TCP aborted (local)";
    case MMIPERF_TCP_ABORTED_LOCAL_DATAERROR: return "TCP aborted (data error)";
    case MMIPERF_TCP_ABORTED_LOCAL_TXERROR: return "TCP aborted (tx error)";
    case MMIPERF_TCP_ABORTED_REMOTE: return "TCP aborted (remote)";
    case MMIPERF_UDP_DONE_SERVER: return "UDP server done";
    case MMIPERF_UDP_DONE_CLIENT: return "UDP client done";
    default: return "interim";
  }
}

/* ------------------------------------------------------------------ */
/* mmiperf callbacks (run in the mmiperf task)                         */
/* ------------------------------------------------------------------ */

static void reportHandler(const struct mmiperf_report *report, void *arg,
                          mmiperf_handle_t handle) {
  (void)arg;
  portENTER_CRITICAL(&s_reportMux);
  /* Only latch the report belonging to the session we are tracking. */
  if (handle && handle == s_tracked) {
    memcpy(&s_report, report, sizeof(struct mmiperf_report));
    s_reportPending = true;
  }
  portEXIT_CRITICAL(&s_reportMux);
}

/* ------------------------------------------------------------------ */

void thrInit(void) {
  memset(&s_res, 0, sizeof(s_res));
  s_res.state = THR_IDLE;
}

void thrStartServers(void) {
  /* Retry each protocol independently, so a partial failure is not latched. */
  if (!s_tcpServer) {
    struct mmiperf_server_args targs = MMIPERF_SERVER_ARGS_DEFAULT;
    targs.local_port = g_cfg.iperfPort;
    targs.report_fn  = reportHandler;
    s_tcpServer = mmiperf_start_tcp_server(&targs);
    if (s_tcpServer) LOGI("IPERF", "TCP server listening on port %u", g_cfg.iperfPort);
    else             LOGE("IPERF", "failed to start TCP server on port %u", g_cfg.iperfPort);
  }
  if (!s_udpServer) {
    struct mmiperf_server_args uargs = MMIPERF_SERVER_ARGS_DEFAULT;
    uargs.local_port = g_cfg.iperfPort;
    uargs.report_fn  = reportHandler;
    s_udpServer = mmiperf_start_udp_server(&uargs);
    if (s_udpServer) LOGI("IPERF", "UDP server listening on port %u", g_cfg.iperfPort);
    else             LOGE("IPERF", "failed to start UDP server on port %u", g_cfg.iperfPort);
  }
}

/* ------------------------------------------------------------------ */
/* Starting a test                                                     */
/* ------------------------------------------------------------------ */

static void resetResult(uint8_t dir, bool udp, uint16_t durationS) {
  memset(&s_res, 0, sizeof(s_res));
  s_res.dir        = dir;
  s_res.udp        = udp ? 1 : 0;
  s_res.requestedS = durationS;
  s_res.startedMs  = millis();
  s_res.state      = THR_ARMING;
}

bool thrStartTest(uint8_t dir, bool udp, uint16_t durationS,
                  uint32_t targetKbps, uint16_t packetSize, String &err) {
  if (s_res.state == THR_ARMING || s_res.state == THR_RUNNING) {
    err = "a test is already running";
    return false;
  }
  if (!halowIsUp()) {
    err = "HaLow link is down";
    return false;
  }
  if (g_cfg.peerIp == 0) {
    err = "peer IP is not configured";
    return false;
  }
  if (durationS == 0) durationS = 10;
  if (durationS > 300) durationS = 300;

  thrStartServers();
  resetResult(dir, udp, durationS);
  s_tracked = NULL;   /* set once we know which session to follow */

  if (dir == THR_DIR_TX) {
    /* Peer already runs persistent servers; just make sure, then be the client. */
    PeerCmdArgs cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd  = PEER_CMD_START_SERVER;
    cmd.udp  = udp ? 1 : 0;
    cmd.port = g_cfg.iperfPort;
    peerSendCommand(cmd);

    memset(&s_pendingArgs, 0, sizeof(s_pendingArgs));
    struct mmiperf_client_args def = MMIPERF_CLIENT_ARGS_DEFAULT;
    s_pendingArgs = def;
    strlcpy(s_pendingArgs.server_addr,
            IPAddress(g_cfg.peerIp).toString().c_str(),
            sizeof(s_pendingArgs.server_addr));
    s_pendingArgs.server_port = g_cfg.iperfPort;
    s_pendingArgs.amount      = -((int32_t)durationS * 100); /* hundredths of a second */
    s_pendingArgs.target_bw   = udp ? targetKbps : 0;
    s_pendingArgs.packet_size = udp ? packetSize : 0;
    s_pendingArgs.report_fn   = reportHandler;

    s_pendingUdp    = udp;
    s_pendingClient = true;
    s_pendingAtMs   = millis() + THR_ARM_DELAY_MS;

    LOGI("IPERF", "TX test: %s to %s:%u for %u s%s",
         udp ? "UDP" : "TCP", s_pendingArgs.server_addr, g_cfg.iperfPort, durationS,
         udp && targetKbps ? " (rate limited)" : "");
  } else {
    /* Ask the peer to transmit at us; our own server will report the result. */
    PeerCmdArgs cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd        = PEER_CMD_START_CLIENT;
    cmd.udp        = udp ? 1 : 0;
    cmd.port       = g_cfg.iperfPort;
    cmd.durationS  = durationS;
    cmd.targetKbps = targetKbps;
    cmd.packetSize = packetSize;
    cmd.targetIp   = (uint32_t)halowLocalIP();

    if (!peerSendCommand(cmd)) {
      s_res.state = THR_FAILED;
      strlcpy(s_res.note, "could not reach peer to start the test", sizeof(s_res.note));
      err = "could not reach peer";
      return false;
    }
    /* Our own server will receive the traffic and produce the report. */
    s_tracked = udp ? s_udpServer : s_tcpServer;
    if (!s_tracked) {
      s_res.state = THR_FAILED;
      strlcpy(s_res.note, "local iperf server is not running", sizeof(s_res.note));
      err = udp ? "UDP server not running" : "TCP server not running";
      return false;
    }
    s_res.state = THR_RUNNING;
    LOGI("IPERF", "RX test: asked peer to send %s for %u s",
         udp ? "UDP" : "TCP", durationS);
  }
  return true;
}

void thrAbort(void) {
  /*
   * mmiperf provides no abort. We stop tracking locally; the underlying
   * session still runs to its configured duration and its report is ignored.
   */
  if (s_res.state == THR_ARMING || s_res.state == THR_RUNNING) {
    s_pendingClient = false;
    s_res.state = THR_DONE;
    strlcpy(s_res.note, "stopped by user; mmiperf session runs to completion",
            sizeof(s_res.note));
    LOGW("IPERF", "test stopped by user (mmiperf cannot abort an active session)");
  }
  /* Stop following the session so its late report cannot land on the next test. */
  portENTER_CRITICAL(&s_reportMux);
  s_tracked = NULL;
  s_reportPending = false;
  portEXIT_CRITICAL(&s_reportMux);
  s_client = NULL;
}

/* ------------------------------------------------------------------ */
/* Peer command handling                                               */
/* ------------------------------------------------------------------ */

static void onPeerCommand(const PeerCmdArgs &args) {
  switch (args.cmd) {
    case PEER_CMD_START_SERVER:
      thrStartServers();
      break;

    case PEER_CMD_START_CLIENT: {
      thrStartServers();
      IPAddress target(args.targetIp);
      uint16_t durationS = args.durationS ? args.durationS : 10;

      struct mmiperf_client_args cargs = MMIPERF_CLIENT_ARGS_DEFAULT;
      strlcpy(cargs.server_addr, target.toString().c_str(), sizeof(cargs.server_addr));
      cargs.server_port = args.port ? args.port : g_cfg.iperfPort;
      cargs.amount      = -((int32_t)durationS * 100);
      cargs.target_bw   = args.udp ? args.targetKbps : 0;
      cargs.packet_size = args.udp ? args.packetSize : 0;
      cargs.report_fn   = reportHandler;

      mmiperf_handle_t h = args.udp ? mmiperf_start_udp_client(&cargs)
                                    : mmiperf_start_tcp_client(&cargs);
      if (h) {
        resetResult(THR_DIR_TX, args.udp != 0, durationS);
        s_client  = h;
        s_tracked = h;
        s_res.state = THR_RUNNING;
        LOGI("IPERF", "peer requested: sending %s to %s:%u for %u s",
             args.udp ? "UDP" : "TCP", cargs.server_addr, cargs.server_port, durationS);
      } else {
        LOGE("IPERF", "failed to start client requested by peer");
      }
      break;
    }

    case PEER_CMD_STOP:
      thrAbort();
      break;

    default:
      break;
  }
}

/* ------------------------------------------------------------------ */
/* Polling                                                             */
/* ------------------------------------------------------------------ */

static void applyFinalReport(const struct mmiperf_report *r) {
  s_res.avgKbps    = r->bandwidth_kbitpsec;
  s_res.bytes      = r->bytes_transferred;
  s_res.durationMs = r->duration_ms;
  s_res.curKbps    = 0;
  s_res.localPort  = r->local_port;
  s_res.remotePort = r->remote_port;
  strlcpy(s_res.localAddr,  r->local_addr,  sizeof(s_res.localAddr));
  strlcpy(s_res.remoteAddr, r->remote_addr, sizeof(s_res.remoteAddr));

  if (s_res.udp) {
    uint32_t total = r->rx_frames + r->error_count;
    /*
     * A UDP *client* report carries no receive-side information: rx_frames and
     * error_count are both zero. Reporting that as "0.00 % loss" would be a
     * fabricated number - and it is exactly the figure a field tester trusts.
     * Loss is only meaningful when the receiving side produced the report.
     */
    s_res.udpStatsValid = (total > 0);
    s_res.txFrames      = r->tx_frames;
    s_res.rxFrames      = r->rx_frames;
    s_res.outOfSeq      = r->out_of_sequence_frames;
    s_res.errorCount    = r->error_count;
    s_res.lossPct100    = total ? (uint16_t)((uint64_t)r->error_count * 10000ULL / total)
                                : SAMPLE_NA_U16;

    if (!s_res.udpStatsValid && s_res.note[0] == '\0') {
      strlcpy(s_res.note, "UDP loss is only reported by the receiving node",
              sizeof(s_res.note));
    }

    if (r->ipg_count > 0) {
      s_res.ipgValid       = true;
      s_res.meanIpgTenthMs = (uint32_t)((uint64_t)r->ipg_sum_ms * 10ULL / r->ipg_count);
    }
  }

  s_res.state = THR_DONE;
  if (s_res.avgKbps) s_lastKbps = s_res.avgKbps;

  LOGI("IPERF", "%s: %lu kbps, %llu bytes in %lu ms",
       reportTypeName(r->report_type), (unsigned long)r->bandwidth_kbitpsec,
       (unsigned long long)r->bytes_transferred, (unsigned long)r->duration_ms);
  if (s_res.udp) {
    if (s_res.udpStatsValid) {
      LOGI("IPERF", "UDP frames tx=%lu rx=%lu lost=%lu out-of-order=%lu (loss %u.%02u%%)",
           (unsigned long)r->tx_frames, (unsigned long)r->rx_frames,
           (unsigned long)r->error_count, (unsigned long)r->out_of_sequence_frames,
           s_res.lossPct100 / 100, s_res.lossPct100 % 100);
    } else {
      LOGI("IPERF", "UDP frames tx=%lu (loss not reported by the sending side)",
           (unsigned long)r->tx_frames);
    }
  }
}

void thrTick(void) {
  /* Deferred client start, so the peer has time to be ready. */
  if (s_pendingClient && (int32_t)(millis() - s_pendingAtMs) >= 0) {
    s_pendingClient = false;
    s_client = s_pendingUdp ? mmiperf_start_udp_client(&s_pendingArgs)
                            : mmiperf_start_tcp_client(&s_pendingArgs);
    if (s_client) {
      s_tracked   = s_client;
      s_res.state = THR_RUNNING;
    } else {
      s_res.state = THR_FAILED;
      strlcpy(s_res.note, "mmiperf client failed to start", sizeof(s_res.note));
      LOGE("IPERF", "mmiperf client failed to start");
    }
  }

  /* Final report from the mmiperf task. */
  struct mmiperf_report r;
  bool haveReport = false;
  portENTER_CRITICAL(&s_reportMux);
  if (s_reportPending) {
    memcpy(&r, &s_report, sizeof(r));
    s_reportPending = false;
    haveReport = true;
  }
  portEXIT_CRITICAL(&s_reportMux);

  if (haveReport) {
    if (s_res.state == THR_RUNNING || s_res.state == THR_ARMING) {
      applyFinalReport(&r);
      s_client  = NULL;
      s_tracked = NULL;
    } else {
      LOGD("IPERF", "%s (no test tracked)", reportTypeName(r.report_type));
    }
  }

  /* Live throughput while a locally-driven test is running. */
  if (s_res.state == THR_RUNNING) {
    uint32_t now = millis();
    if ((uint32_t)(now - s_lastPoll) >= THR_POLL_MS) {
      s_lastPoll = now;

      /*
       * Only poll our own client handle. mmiperf.h states an interim report
       * must not be requested after that session's final callback, and the
       * iperf servers are persistent across many sessions - polling them
       * would return a previous session's counters. RX tests therefore report
       * their result when the final report arrives, with no live figure.
       */
      mmiperf_handle_t h = s_client;
      struct mmiperf_report ir;
      if (h && mmiperf_get_interim_report(h, &ir)) {
        s_res.curKbps = ir.bandwidth_kbitpsec;
        s_res.bytes   = ir.bytes_transferred;
        if (ir.bandwidth_kbitpsec) s_lastKbps = ir.bandwidth_kbitpsec;
      }
    }

    /* Safety net: never leave the UI stuck in "running". */
    uint32_t limit = (uint32_t)s_res.requestedS * 1000UL + THR_GRACE_MS;
    if ((uint32_t)(millis() - s_res.startedMs) > limit) {
      s_res.state = THR_DONE;
      if (s_res.note[0] == '\0') {
        strlcpy(s_res.note, "timed out waiting for the final report", sizeof(s_res.note));
      }
      LOGW("IPERF", "test timed out waiting for the final report");
      s_client  = NULL;
      portENTER_CRITICAL(&s_reportMux);
      s_tracked = NULL;
      portEXIT_CRITICAL(&s_reportMux);
    }
  }
}

const ThroughputResult &thrResult(void) {
  return s_res;
}

uint32_t thrLastKbps(void) {
  if (s_res.state == THR_RUNNING && s_res.curKbps) return s_res.curKbps;
  return s_lastKbps;
}

/* Registered from setup(); kept here so peer_link stays independent. */
void thrRegisterPeerHandler(void) {
  peerSetCommandHandler(onPeerCommand);
}
