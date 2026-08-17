#include "link_monitor.h"
#include "halow_manager.h"
#include "mcs_table.h"
#include "stats.h"
#include "config.h"
#include "logbuf.h"
#include "rtt_test.h"
#include "throughput_test.h"
#include "peer_link.h"

#include "mmwlan.h"
#include "mmwlan_stats.h"

#define SAMPLE_PERIOD_MS 1000
#define RATE_STALE_MS    15000

static LinkStats s_stats;
static uint32_t  s_lastSample = 0;

/* Previous rate-table snapshot, so we can work on deltas. */
static uint32_t *s_prevSent = NULL;
static uint32_t *s_prevSucc = NULL;
static uint32_t  s_prevN    = 0;
static uint32_t  s_lastRateMoveMs = 0;
/*
 * False until a snapshot has been taken, so the first pass after (re)allocating
 * the arrays does not treat the lifetime counters as a one-second delta - that
 * would publish the most-used-ever rate as "current" and a lifetime PER.
 */
static bool      s_baselineValid = false;

void linkMonitorInit(void) {
  memset(&s_stats, 0, sizeof(s_stats));
  s_stats.mcs = SAMPLE_NA_MCS;
  histInit();
  LOGI("LINK", "link monitor started (%u ms sampling)", (unsigned)SAMPLE_PERIOD_MS);
}

void linkMonitorReset(void) {
  free(s_prevSent); free(s_prevSucc);
  s_prevSent = s_prevSucc = NULL;
  s_prevN = 0;
  s_baselineValid  = false;
  s_lastRateMoveMs = 0;
  histClear();
  memset(&s_stats, 0, sizeof(s_stats));
  s_stats.mcs = SAMPLE_NA_MCS;
  LOGI("LINK", "statistics and history cleared");
}

/* ------------------------------------------------------------------ */
/* Rate control table -> current MCS / PHY rate / PER                  */
/* ------------------------------------------------------------------ */

static void sampleRateControl(void) {
  struct mmwlan_rc_stats *rc = mmwlan_get_rc_stats();
  if (!rc) {
    return; /* leave previous values in place; rateAgeMs will grow */
  }

  const uint32_t n = rc->n_entries;

  /* Resize the snapshot if the table geometry changed. */
  if (n != s_prevN) {
    free(s_prevSent); free(s_prevSucc);
    s_prevSent = (uint32_t *)calloc(n, sizeof(uint32_t));
    s_prevSucc = (uint32_t *)calloc(n, sizeof(uint32_t));
    s_prevN    = (s_prevSent && s_prevSucc) ? n : 0;
    s_baselineValid = false;
  }

  if (s_prevN == n && n > 0 && s_baselineValid) {
    uint32_t bestDelta = 0;
    uint32_t bestIdx   = 0;
    uint32_t totSent   = 0;
    uint32_t totSucc   = 0;

    for (uint32_t i = 0; i < n; i++) {
      /* Counters are cumulative and can be reset by the driver; clamp. */
      uint32_t dSent = (rc->total_sent[i]    >= s_prevSent[i]) ? rc->total_sent[i]    - s_prevSent[i] : 0;
      uint32_t dSucc = (rc->total_success[i] >= s_prevSucc[i]) ? rc->total_success[i] - s_prevSucc[i] : 0;
      totSent += dSent;
      totSucc += dSucc;
      if (dSent > bestDelta) {
        bestDelta = dSent;
        bestIdx   = i;
      }
    }

    if (totSent > 0) {
      uint32_t ri = rc->rate_info[bestIdx];
      s_stats.mcs         = (int8_t)RC_RATE_INFO_MCS(ri);
      s_stats.bwMhz       = rcBwFieldToMhz(RC_RATE_INFO_BW(ri));
      s_stats.shortGi     = RC_RATE_INFO_SGI(ri) != 0;
      s_stats.phyRateKbps = phyRateKbps((uint8_t)s_stats.mcs, s_stats.bwMhz, s_stats.shortGi);
      s_stats.rateValid   = true;
      s_lastRateMoveMs    = millis();

      /* PER over this interval only. */
      uint32_t failed = (totSent > totSucc) ? (totSent - totSucc) : 0;
      s_stats.perPct100 = (uint16_t)((uint64_t)failed * 10000ULL / totSent);
      s_stats.perValid  = true;
    } else {
      /* No frames sent in this interval: nothing new to report. */
      s_stats.perValid = false;
    }
  }

  /* Cumulative totals across the whole table. */
  uint32_t cumSent = 0, cumSucc = 0;
  for (uint32_t i = 0; i < n; i++) {
    cumSent += rc->total_sent[i];
    cumSucc += rc->total_success[i];
    if (s_prevN == n) {
      s_prevSent[i] = rc->total_sent[i];
      s_prevSucc[i] = rc->total_success[i];
    }
  }
  if (s_prevN == n && n > 0) {
    s_baselineValid = true;   /* deltas are meaningful from the next sample on */
  }
  s_stats.framesAttempted = cumSent;
  s_stats.framesSucceeded = cumSucc;

  mmwlan_free_rc_stats(rc);

  s_stats.rateAgeMs = s_lastRateMoveMs ? (millis() - s_lastRateMoveMs) : 0;
  if (s_lastRateMoveMs && s_stats.rateAgeMs > RATE_STALE_MS) {
    s_stats.rateValid = false;   /* too old to present as "current" */
  }
}

/* ------------------------------------------------------------------ */

static void sampleUmac(void) {
  struct mmwlan_stats_umac_data u;
  memset(&u, 0, sizeof(u));
  if (mmwlan_get_umac_stats(&u) != MMWLAN_SUCCESS) {
    s_stats.umacValid = false;
    return;
  }
  s_stats.umacValid         = true;
  s_stats.umacRssi          = u.rssi;
  s_stats.txqDropped        = u.datapath_txq_frames_dropped;
  s_stats.rxqDropped        = u.datapath_rxq_frames_dropped;
  s_stats.rxCcmpFailures    = u.datapath_rx_ccmp_failures;
  s_stats.rxAllocFailures   = u.datapath_driver_rx_alloc_failures;
  s_stats.rxReorderTimedout = u.datapath_rx_reorder_timedout;
  s_stats.hwRestarts        = u.hw_restart_counter;
}

static void sampleDutyCycle(void) {
  struct mmwlan_duty_cycle_stats d;
  memset(&d, 0, sizeof(d));
  if (mmwlan_get_duty_cycle_stats(&d) != MMWLAN_SUCCESS) {
    s_stats.dutyValid = false;
    return;
  }
  s_stats.dutyValid       = true;
  s_stats.dutyCyclePct100 = d.duty_cycle;
  s_stats.dutyMode        = (uint8_t)d.mode;
}

static void sampleRssi(void) {
  /*
   * Only the STA measures a meaningful RSSI (the signal from the AP).
   * In AP role we leave it invalid; the panel shows the peer-reported value.
   */
  if (g_cfg.role != ROLE_STA) {
    s_stats.rssiValid = false;
    return;
  }
  int32_t r = mmwlan_get_rssi();
  if (r == INT32_MIN) {
    s_stats.rssiValid = false;
  } else {
    s_stats.rssiValid = true;
    s_stats.rssiDbm   = (int16_t)r;
  }
}

void linkMonitorTick(void) {
  uint32_t now = millis();
  if ((uint32_t)(now - s_lastSample) < SAMPLE_PERIOD_MS) return;
  s_lastSample = now;

  s_stats.linkUp       = halowIsUp();
  s_stats.linkUptimeMs = halowLinkUptimeMs();
  s_stats.disconnects  = halowDisconnectCount();

  if (s_stats.linkUp) {
    sampleRssi();
    sampleRateControl();
    sampleUmac();
    sampleDutyCycle();
  } else {
    s_stats.rssiValid = false;
    s_stats.rateValid = false;
    s_stats.perValid  = false;
  }

  /* ---- build the history sample ---- */
  Sample s;
  memset(&s, 0, sizeof(s));
  s.tMs   = now;
  s.flags = s_stats.linkUp ? SF_LINK_UP : 0;

  /* Prefer our own RSSI; on the AP node fall back to the peer's reading. */
  if (s_stats.rssiValid) {
    s.rssi = s_stats.rssiDbm;
  } else if (peerIsFresh() && peerData().rssiValid) {
    s.rssi   = peerData().rssiDbm;
    s.flags |= SF_PEER_OK;
  } else {
    s.rssi = SAMPLE_NA_RSSI;
  }

  if (s_stats.rateValid) {
    s.mcs     = s_stats.mcs;
    s.bwMhz   = s_stats.bwMhz;
    s.phyKbps = s_stats.phyRateKbps;
    if (s_stats.shortGi) s.flags |= SF_SGI;
  } else if (peerIsFresh() && peerData().rateValid) {
    s.mcs     = peerData().mcs;
    s.bwMhz   = peerData().bwMhz;
    s.phyKbps = peerData().phyRateKbps;
    s.flags  |= SF_PEER_OK;
  } else {
    s.mcs = SAMPLE_NA_MCS;
  }

  s.perPct100  = s_stats.perValid ? s_stats.perPct100 : SAMPLE_NA_U16;
  s.rttTenthMs = rttLastTenthMs();
  s.lossPct100 = rttLossPct100();
  s.thrKbps    = thrLastKbps();

  histPush(s);
}

const LinkStats &linkStats(void) {
  return s_stats;
}
