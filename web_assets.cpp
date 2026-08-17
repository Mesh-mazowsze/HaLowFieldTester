#include "web_assets.h"

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>HaLow Field Tester</title>
<link rel="stylesheet" href="/style.css">
</head><body>

<header>
  <div class="hdr-row">
    <div>
      <div class="hdr-title">HaLow Field Tester</div>
      <div class="hdr-sub" id="hdrSub">connecting…</div>
    </div>
    <div class="pill" id="linkPill">--</div>
  </div>
</header>

<nav id="tabs">
  <button class="tab active" data-tab="dash">Dashboard</button>
  <button class="tab" data-tab="charts">Charts</button>
  <button class="tab" data-tab="tests">Tests</button>
  <button class="tab" data-tab="config">Config</button>
  <button class="tab" data-tab="logs">Logs</button>
</nav>

<main>

<!-- ================= DASHBOARD ================= -->
<section id="dash" class="page active">

  <div class="card live">
    <h2>Live link</h2>
    <div class="live-grid">
      <div class="metric"><span class="lbl">Connected</span><span class="val" id="mConn">--</span></div>
      <div class="metric"><span class="lbl">RSSI</span><span class="val" id="mRssi">--</span></div>
      <div class="metric"><span class="lbl">MCS</span><span class="val" id="mMcs">--</span></div>
      <div class="metric"><span class="lbl">PHY rate</span><span class="val" id="mPhy">--</span></div>
      <div class="metric"><span class="lbl">TX power</span><span class="val" id="mTxp">--</span></div>
      <div class="metric"><span class="lbl">PHY error rate</span><span class="val" id="mPer">--</span></div>
      <div class="metric"><span class="lbl">RTT</span><span class="val" id="mRtt">--</span></div>
      <div class="metric"><span class="lbl">Probe loss</span><span class="val" id="mLoss">--</span></div>
      <div class="metric"><span class="lbl">Throughput</span><span class="val" id="mThr">--</span></div>
    </div>
    <div class="rssi-bar"><div id="rssiFill"></div></div>
    <div class="hint" id="rssiSrc"></div>
  </div>

  <div class="card">
    <h2>Peer node <span class="sub" id="peerFresh"></span></h2>
    <table class="kv" id="tPeer"></table>
  </div>

  <div class="card">
    <h2>HaLow configuration</h2>
    <table class="kv" id="tHalow"></table>
  </div>

  <div class="card">
    <h2>Link statistics</h2>
    <table class="kv" id="tLink"></table>
    <div class="hint">RSSI and the rate table are measured by the <em>station</em>.
      On the AP node these arrive over the peer telemetry link and are marked
      <em>(peer)</em>. A dash means the value is genuinely unavailable from the
      MM6108 API — it is never estimated.</div>
  </div>

  <div class="card">
    <h2>Device</h2>
    <table class="kv" id="tDev"></table>
  </div>

  <div class="card">
    <h2>Radio / firmware</h2>
    <table class="kv" id="tRadio"></table>
  </div>

  <div class="card warn">
    <strong>Regulatory notice</strong>
    <p>TX power and channel configuration must comply with local regulations.
    This device is configured for the EU 863–868&nbsp;MHz band. The 868&nbsp;MHz
    band is duty-cycle limited; you are responsible for lawful operation.</p>
  </div>
</section>

<!-- ================= CHARTS ================= -->
<section id="charts" class="page">
  <div class="card">
    <div class="row">
      <label>Window</label>
      <select id="histWindow">
        <option value="0:300">5 minutes</option>
        <option value="0:900" selected>15 minutes</option>
        <option value="1:3600">60 minutes</option>
      </select>
      <button class="btn small" id="btnRefreshHist">Refresh</button>
      <button class="btn small ghost" id="btnStatsReset">Clear history</button>
    </div>
    <div class="hint">Held in RAM only; cleared on reboot.</div>
  </div>

  <div class="card"><h2>RSSI (dBm)</h2><canvas id="cRssi" height="150"></canvas></div>
  <div class="card"><h2>Throughput (Mbps)</h2><canvas id="cThr" height="150"></canvas></div>
  <div class="card"><h2>RTT (ms)</h2><canvas id="cRtt" height="150"></canvas></div>
  <div class="card"><h2>Packet loss (%)</h2><canvas id="cLoss" height="150"></canvas></div>
  <div class="card"><h2>MCS</h2><canvas id="cMcs" height="150"></canvas></div>
</section>

<!-- ================= TESTS ================= -->
<section id="tests" class="page">

  <div class="card">
    <h2>Throughput test</h2>
    <div class="grid2">
      <div><label>Protocol</label>
        <select id="tProto"><option value="tcp">TCP</option><option value="udp">UDP</option></select></div>
      <div><label>Direction</label>
        <select id="tDir"><option value="tx">TX (this node sends)</option>
                          <option value="rx">RX (this node receives)</option></select></div>
      <div><label>Duration (s)</label><input type="number" id="tDur" value="10" min="1" max="300"></div>
      <div><label>UDP target (kbps)</label><input type="number" id="tRate" value="2000" min="0"></div>
      <div><label>UDP packet size</label><input type="number" id="tPkt" value="0" min="0" max="1460"></div>
    </div>
    <div class="hint">Packet size 0 uses the mmiperf default (1460 B for IPv4).
      Target 0 means unlimited. Both apply to UDP only.</div>
    <div class="row">
      <button class="btn" id="btnTestStart">Start test</button>
      <button class="btn ghost" id="btnTestStop">Stop</button>
    </div>
    <table class="kv" id="tThr"></table>
  </div>

  <div class="card">
    <h2>Ping / RTT</h2>
    <div class="row">
      <label>Continuous probe</label>
      <input type="checkbox" id="pEnable">
      <label>Interval (ms)</label>
      <input type="number" id="pInterval" value="1000" min="100" max="60000" style="width:7em">
      <button class="btn small" id="btnPingApply">Apply</button>
      <button class="btn small ghost" id="btnPingReset">Reset stats</button>
    </div>
    <div class="hint">UDP echo between the two nodes, not ICMP. Doubles as the
      continuous link test: one small packet per interval, so you can drive or
      walk and watch the link without running full iperf traffic.</div>
    <table class="kv" id="tPing"></table>
  </div>

  <div class="card">
    <h2>Export</h2>
    <div class="row">
      <a class="btn small" href="/api/export.csv?window=0" download>Download CSV</a>
      <a class="btn small" href="/api/export.json?window=0" download>Download JSON</a>
    </div>
    <div class="hint">Fields the MM6108 API cannot provide (SNR, noise floor,
      actual TX power, HaLow packet counters) are present in the schema but
      always empty — never invented.</div>
  </div>
</section>

<!-- ================= CONFIG ================= -->
<section id="config" class="page">
  <form id="cfgForm">
    <div class="card">
      <h2>Role</h2>
      <div class="row">
        <label><input type="radio" name="role" value="0"> HaLow AP</label>
        <label><input type="radio" name="role" value="1"> HaLow STA</label>
      </div>
      <div class="hint">Changing the role requires a reboot. The AP normally
        takes the gateway address; the STA a different address in the same subnet.</div>
    </div>

    <div class="card">
      <h2>HaLow radio</h2>
      <div class="grid2">
        <div><label>SSID</label><input name="halow_ssid" maxlength="32"></div>
        <div><label>Passphrase</label><input name="halow_pass" maxlength="64"></div>
        <div><label>Security</label>
          <select name="security"><option value="1">SAE</option><option value="0">Open</option></select></div>
        <div><label>Region</label><select name="region" id="cfgRegion"></select></div>
        <div><label>Channel</label><select name="channel" id="cfgChannel"></select></div>
        <div><label>Max TX power (dBm, 0 = regulatory max)</label>
          <input type="number" name="txpower" min="0" max="30"></div>
        <div><label>AP beacon interval (TU, 0 = auto)</label>
          <input type="number" name="beacon_tus" min="0" max="10000"></div>
      </div>
      <div class="hint"><strong>Beacon interval matters on EU 1 MHz.</strong> A
        beacon at 1 MHz needs ~4–5&nbsp;ms of airtime; at the 100&nbsp;TU default
        that exceeds the EU 2.80&nbsp;% duty cycle, the driver stops sending
        beacons and no station can find the AP. Auto stretches it to
        300&nbsp;TU on duty-cycle-limited 1 MHz channels.</div>
      <div class="hint" id="chanHint"></div>
      <div class="hint"><strong>Note:</strong> the Morse API can only lower the
        regulatory maximum for a channel, never raise it.</div>
    </div>

    <div class="card">
      <h2>HaLow IP (static — the HaLow AP has no DHCP server)</h2>
      <div class="grid2">
        <div><label>IP address</label><input name="ip"></div>
        <div><label>Netmask</label><input name="netmask"></div>
        <div><label>Gateway</label><input name="gateway"></div>
        <div><label>Peer IP</label><input name="peer_ip"></div>
      </div>
    </div>

    <div class="card">
      <h2>Management Wi-Fi (2.4 GHz)</h2>
      <div class="grid2">
        <div><label>SSID (blank = auto)</label><input name="mgmt_ssid" maxlength="32"></div>
        <div><label>Password (min 8 chars)</label><input name="mgmt_pass" maxlength="64"></div>
        <div><label>Channel</label><input type="number" name="mgmt_channel" min="1" max="13"></div>
        <div><label>Forwarding to HaLow</label>
          <select name="forward_mode">
            <option value="0">Isolated (recommended)</option>
            <option value="1">NAT</option>
            <option value="2">Route (no NAT)</option>
          </select></div>
      </div>
      <div class="hint"><strong>Isolated</strong> gives panel clients an address
        but no default route, so a phone left connected cannot push background
        traffic over the HaLow link and distort your measurements. Choose
        <strong>NAT</strong> to reach the peer's panel or the internet through
        the link — simple, but it stacks into double NAT when both nodes route.
        <strong>Route</strong> forwards without rewriting addresses (no double
        NAT, end-to-end addressing) but the upstream router needs a static route
        back to this subnet.</div>
    </div>

    <div class="card">
      <h2>Ports</h2>
      <div class="grid2">
        <div><label>iperf port</label><input type="number" name="iperf_port" min="1" max="65535"></div>
        <div><label>RTT port</label><input type="number" name="rtt_port" min="1" max="65535"></div>
        <div><label>Peer telemetry port</label><input type="number" name="peer_port" min="1" max="65535"></div>
      </div>
    </div>

    <div class="card">
      <div class="row">
        <button class="btn" type="submit">Save</button>
        <button class="btn ghost" type="button" id="btnSaveReboot">Save &amp; reboot</button>
        <button class="btn ghost" type="button" id="btnReboot">Reboot</button>
      </div>
      <div id="cfgMsg" class="hint"></div>
    </div>
  </form>
</section>

<!-- ================= LOGS ================= -->
<section id="logs" class="page">
  <div class="card">
    <div class="row">
      <button class="btn small" id="btnLogRefresh">Refresh</button>
      <button class="btn small ghost" id="btnLogClear">Clear</button>
      <a class="btn small" href="/api/log.txt" download>Download</a>
      <label class="row" style="margin-left:auto"><input type="checkbox" id="logAuto" checked> auto</label>
    </div>
    <pre id="logView"></pre>
  </div>
</section>

</main>
<script src="/app.js"></script>
</body></html>
)HTML";

const char APP_CSS[] PROGMEM = R"CSS(
*{box-sizing:border-box}
:root{
  --bg:#0f1419; --card:#182028; --line:#2a3540; --fg:#e6edf3; --dim:#93a1b0;
  --acc:#3fb950; --warn:#d29922; --bad:#f85149; --blue:#58a6ff;
}
html,body{margin:0;padding:0}
body{background:var(--bg);color:var(--fg);
  font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  padding-bottom:2rem}
header{position:sticky;top:0;z-index:10;background:#0f1419ee;backdrop-filter:blur(8px);
  border-bottom:1px solid var(--line);padding:.6rem .8rem}
.hdr-row{display:flex;align-items:center;gap:.8rem;max-width:900px;margin:0 auto}
.hdr-title{font-weight:700;font-size:1.05rem}
.hdr-sub{color:var(--dim);font-size:.78rem}
.pill{margin-left:auto;padding:.25rem .7rem;border-radius:999px;font-size:.78rem;
  font-weight:600;background:#30363d;white-space:nowrap}
.pill.up{background:#1a4d2a;color:var(--acc)}
.pill.down{background:#4d1a1a;color:var(--bad)}

nav{display:flex;gap:.25rem;overflow-x:auto;padding:.5rem .8rem;max-width:900px;margin:0 auto;
  -webkit-overflow-scrolling:touch}
.tab{flex:0 0 auto;background:#1c242c;color:var(--dim);border:1px solid var(--line);
  padding:.45rem .85rem;border-radius:8px;font-size:.85rem;cursor:pointer}
.tab.active{background:var(--blue);color:#0b1117;border-color:var(--blue);font-weight:600}

main{max-width:900px;margin:0 auto;padding:0 .8rem}
.page{display:none}.page.active{display:block}

.card{background:var(--card);border:1px solid var(--line);border-radius:12px;
  padding:.85rem;margin:.7rem 0}
.card h2{margin:0 0 .6rem;font-size:.95rem;letter-spacing:.02em}
.card h2 .sub{font-weight:400;color:var(--dim);font-size:.78rem}
.card.warn{border-color:#5c4415;background:#241d0d}
.card.warn p{margin:.4rem 0 0;font-size:.83rem;color:#e3c88a}

.live-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:.5rem}
.metric{background:#111820;border:1px solid var(--line);border-radius:9px;padding:.5rem .6rem}
.metric .lbl{display:block;color:var(--dim);font-size:.7rem;text-transform:uppercase;
  letter-spacing:.05em}
.metric .val{display:block;font-size:1.15rem;font-weight:650;margin-top:.15rem;
  font-variant-numeric:tabular-nums}
.val.na{color:var(--dim);font-weight:400;font-size:.95rem}
.val.good{color:var(--acc)}.val.mid{color:var(--warn)}.val.bad{color:var(--bad)}

.rssi-bar{height:7px;background:#111820;border-radius:4px;margin-top:.7rem;overflow:hidden}
.rssi-bar>div{height:100%;width:0;background:linear-gradient(90deg,#f85149,#d29922,#3fb950);
  transition:width .4s}

table.kv{width:100%;border-collapse:collapse;font-size:.85rem}
table.kv td{padding:.32rem .2rem;border-bottom:1px solid #222c35;vertical-align:top}
table.kv td:first-child{color:var(--dim);width:48%}
table.kv td:last-child{text-align:right;font-variant-numeric:tabular-nums;
  word-break:break-word}
td.na{color:#5d6b78;font-style:italic}

.hint{color:var(--dim);font-size:.76rem;margin-top:.45rem;line-height:1.4}
.row{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;margin:.5rem 0}
.grid2{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:.6rem}
label{display:block;color:var(--dim);font-size:.76rem;margin-bottom:.15rem}
input,select{width:100%;background:#0d1319;color:var(--fg);border:1px solid var(--line);
  border-radius:7px;padding:.45rem .5rem;font-size:.9rem;font-family:inherit}
input[type=checkbox],input[type=radio]{width:auto;margin-right:.3rem}
.row label{display:inline-flex;align-items:center;margin:0;font-size:.85rem;color:var(--fg)}

.btn{background:var(--blue);color:#0b1117;border:0;border-radius:8px;padding:.5rem 1rem;
  font-size:.88rem;font-weight:600;cursor:pointer;text-decoration:none;display:inline-block}
.btn.small{padding:.35rem .7rem;font-size:.8rem}
.btn.ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}
.btn:disabled{opacity:.45}

canvas{width:100%;display:block}
pre#logView{background:#0b1016;border:1px solid var(--line);border-radius:8px;padding:.6rem;
  font-size:.72rem;line-height:1.35;max-height:60vh;overflow:auto;white-space:pre-wrap;
  word-break:break-word;margin:.5rem 0 0}
)CSS";

const char APP_JS[] PROGMEM = R"JS(
'use strict';
const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));

/* ---------- helpers: never invent a value ---------- */
const NA = '—';
function has(v){ return v !== null && v !== undefined && v !== ''; }
function fmt(v, unit, dec){
  if(!has(v)) return NA;
  let n = (typeof v === 'number') ? (dec===undefined ? v : v.toFixed(dec)) : v;
  return unit ? n + ' ' + unit : String(n);
}
function setVal(id, v, unit, dec, cls){
  const el = $(id); if(!el) return;
  el.className = 'val';
  if(!has(v)){ el.textContent = NA; el.classList.add('na'); return; }
  el.textContent = fmt(v, unit, dec);
  if(cls) el.classList.add(cls);
}
/* Values come from the device (SSID, MAC, notes) - escape before inserting. */
function esc(s){
  return String(s).replace(/[&<>"']/g, c =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
function rows(tableId, pairs){
  const t = $(tableId); if(!t) return;
  t.innerHTML = pairs.map(([k,v]) => {
    const na = !has(v);
    return '<tr><td>'+esc(k)+'</td><td'+(na?' class="na"':'')+'>'+
           (na?NA:esc(v))+'</td></tr>';
  }).join('');
}
function mbps(kbps){ return has(kbps) ? (kbps/1000).toFixed(2) : null; }
function dur(sec){
  if(!has(sec)) return null;
  sec = Math.floor(sec);
  const d=Math.floor(sec/86400), h=Math.floor(sec%86400/3600),
        m=Math.floor(sec%3600/60), s=sec%60;
  return (d?d+'d ':'') + String(h).padStart(2,'0')+':'+
         String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');
}
function rssiClass(r){ return r>=-70?'good' : r>=-90?'mid' : 'bad'; }

/* ---------- tabs ---------- */
$$('.tab').forEach(b => b.onclick = () => {
  $$('.tab').forEach(x=>x.classList.remove('active'));
  $$('.page').forEach(x=>x.classList.remove('active'));
  b.classList.add('active');
  $('#'+b.dataset.tab).classList.add('active');
  if(b.dataset.tab==='charts') loadHistory();
  if(b.dataset.tab==='logs') loadLog();
  if(b.dataset.tab==='config') loadConfig();
});

/* ---------- live status ---------- */
let statusCache = null;

function renderStatus(d){
  statusCache = d;
  const L = d.link || {}, H = d.halow || {}, P = d.peer || {}, T = d.throughput || {};

  $('#hdrSub').textContent = (d.device.hostname||'') + ' · role ' + H.role +
      ' · fw ' + d.device.fw_version;
  const pill = $('#linkPill');
  pill.textContent = L.up ? 'LINK UP' : 'LINK DOWN';
  pill.className = 'pill ' + (L.up ? 'up':'down');

  /* ---- live metrics ---- */
  $('#mConn').textContent = L.up ? 'YES' : 'NO';
  $('#mConn').className = 'val ' + (L.up?'good':'bad');

  /* RSSI: our own if we have it, otherwise the peer's (labelled). */
  let rssi = L.rssi_dbm, src = 'measured locally (STA side)';
  if(!has(rssi) && has(P.rssi_dbm)){ rssi = P.rssi_dbm; src = 'reported by peer node'; }
  setVal('#mRssi', rssi, 'dBm', 0, has(rssi)?rssiClass(rssi):null);
  $('#rssiSrc').textContent = has(rssi) ? ('RSSI source: ' + src) :
      'RSSI is measured by the STA node only; none available yet.';
  const pct = has(rssi) ? Math.max(0, Math.min(100, (rssi + 105) * (100/50))) : 0;
  $('#rssiFill').style.width = pct + '%';

  let mcs = has(L.mcs) ? L.mcs : P.mcs;
  let phy = has(L.phy_rate_kbps) ? L.phy_rate_kbps : P.phy_rate_kbps;
  setVal('#mMcs', mcs);
  setVal('#mPhy', mbps(phy), 'Mbps');
  setVal('#mTxp', has(H.tx_power_dbm) ? H.tx_power_dbm : null, 'dBm');
  setVal('#mPer', has(L.phy_per_pct) ? L.phy_per_pct : null, '%', 2);
  setVal('#mRtt', d.rtt && d.rtt.valid ? d.rtt.last_ms : null, 'ms', 1);
  setVal('#mLoss', d.rtt ? d.rtt.loss_pct : null, '%', 2);
  setVal('#mThr', mbps(T.state===2 ? T.current_kbps : T.avg_kbps), 'Mbps');

  /* ---- peer ---- */
  $('#peerFresh').textContent = P.fresh ? '(live)' : (P.valid ? '(stale)' : '(no data)');
  rows('#tPeer', [
    ['Role',            P.role],
    ['IP',              P.ip],
    ['HaLow MAC',       P.mac],
    ['Link',            has(P.link_up) ? (P.link_up?'up':'down') : null],
    ['RSSI',            fmtOrNull(P.rssi_dbm,' dBm')],
    ['MCS',             P.mcs],
    ['PHY rate',        has(P.phy_rate_kbps)? mbps(P.phy_rate_kbps)+' Mbps' : null],
    ['Link uptime',     dur(P.link_uptime_s)],
    ['Uptime',          dur(P.uptime_s)],
    ['Free heap',       has(P.free_heap)? (P.free_heap/1024).toFixed(1)+' KiB' : null],
    ['Firmware',        P.fw],
  ]);

  /* ---- HaLow config ---- */
  rows('#tHalow', [
    ['Role',             H.role],
    ['Region',           H.region],
    ['Channel',          H.channel],
    ['Centre frequency', has(H.centre_freq_hz)? (H.centre_freq_hz/1e6).toFixed(1)+' MHz' : null],
    ['Bandwidth',        has(H.bw_mhz)? H.bw_mhz+' MHz' : null],
    ['Operating classes',has(H.s1g_op_class)? ('S1G '+H.s1g_op_class+' / global '+H.global_op_class) : null],
    ['Regulatory max EIRP', has(H.max_eirp_dbm)? H.max_eirp_dbm+' dBm' : null],
    ['Configured TX power', has(H.tx_power_dbm)? H.tx_power_dbm+' dBm'+(H.tx_power_is_override?' (override)':' (regulatory max)') : null],
    ['Regulatory duty cycle', has(H.duty_cycle_pct)? H.duty_cycle_pct+' %' : null],
    ['AP beacon interval', has(H.beacon_interval_tus)?
        H.beacon_interval_tus+' TU ('+Math.round(H.beacon_interval_tus*1.024)+' ms)' : null],
    ['STA scan dwell',   has(H.scan_dwell_ms)? H.scan_dwell_ms+' ms' : null],
    ['SSID',             H.ssid],
    ['Security',         H.security],
    ['IP address',       H.ip],
    ['Netmask',          H.netmask],
    ['Gateway',          H.gateway],
    ['Peer IP',          H.peer_ip],
    ['HaLow MAC',        H.mac],
    ['Peer/AP MAC (BSSID)', H.peer_mac],
  ]);

  /* ---- link stats ---- */
  rows('#tLink', [
    ['State',              L.up ? 'associated' : 'down'],
    ['Association uptime', dur(L.uptime_s)],
    ['Disconnects',        L.disconnects],
    ['RSSI',               localOrPeer(L.rssi_dbm, P.rssi_dbm, ' dBm')],
    ['SNR',                null],
    ['Noise floor',        null],
    ['MCS',                localOrPeer(L.mcs, P.mcs, '')],
    ['Bandwidth',          localOrPeer(L.bw_mhz, P.bw_mhz, ' MHz')],
    ['Guard interval',     has(L.short_gi)? (L.short_gi?'short':'long') : null],
    ['PHY rate',           localOrPeer(has(L.phy_rate_kbps)? mbps(L.phy_rate_kbps) : null,
                                       has(P.phy_rate_kbps)? mbps(P.phy_rate_kbps) : null, ' Mbps')],
    ['PHY error rate',     has(L.phy_per_pct)? L.phy_per_pct.toFixed(2)+' %' : null],
    ['Rate reading age',   has(L.rate_age_ms)? L.rate_age_ms+' ms' : null],
    ['Frames attempted',   L.frames_attempted],
    ['Frames succeeded',   L.frames_succeeded],
    ['UMAC RSSI',          has(L.umac_rssi_dbm)? L.umac_rssi_dbm+' dBm' : null],
    ['TX queue drops',     L.txq_dropped],
    ['RX queue drops',     L.rxq_dropped],
    ['RX CCMP failures',   L.rx_ccmp_failures],
    ['RX alloc failures',  L.rx_alloc_failures],
    ['RX reorder timeouts',L.rx_reorder_timedout],
    ['HW restarts',        L.hw_restarts],
    ['Duty cycle target',  has(L.duty_cycle_pct)? L.duty_cycle_pct+' %' : null],
    ['TX packets',         null],
    ['RX packets',         null],
    ['Disconnect reason',  null],
    ['Channel utilisation',null],
  ]);

  /* ---- device ---- */
  const D = d.device;
  rows('#tDev', [
    ['Hostname',      D.hostname],
    ['Firmware',      D.fw_version],
    ['Uptime',        dur(D.uptime_s)],
    ['Free heap',     (D.free_heap/1024).toFixed(1)+' KiB'],
    ['Min free heap', has(D.min_free_heap)? (D.min_free_heap/1024).toFixed(1)+' KiB' : null],
    ['PSRAM free',    has(D.free_psram)? (D.free_psram/1024).toFixed(1)+' KiB' : null],
    ['CPU temperature', has(D.temperature_c)? D.temperature_c.toFixed(1)+' °C' : null],
    ['Wi-Fi MAC (mgmt)', D.wifi_mac],
    ['HaLow MAC',     D.halow_mac],
    ['Mgmt SSID',     D.mgmt_ssid],
    ['Forwarding',    D.forward_mode],
    ['Mgmt clients',  D.mgmt_clients],
    ['Reset reason',  D.reset_reason],
  ]);

  /* ---- radio ---- */
  const R = d.radio;
  rows('#tRadio', [
    ['MM6108 firmware',   R.morse_fw_version],
    ['Morselib version',  R.morselib_version],
    ['Chip ID',           R.chip_id_string],
    ['Chip ID (raw)',     has(R.chip_id)? '0x'+R.chip_id.toString(16).toUpperCase() : null],
    ['BCF version',       R.bcf_version],
    ['BCF board',         R.bcf_board_desc],
    ['BCF build',         R.bcf_build_version],
    ['Morse SDK',         R.sdk_version],
    ['Regulatory domain', R.regulatory_domain],
  ]);

  /* ---- throughput panel ---- */
  renderThr(T);
  renderPing(d.rtt);
}

function fmtOrNull(v, unit){ return has(v) ? (v + (unit||'')) : null; }

/*
 * RSSI and the rate table are STA-side measurements, so on the AP node they
 * are legitimately absent locally. Rather than showing a bare dash, fall back
 * to the value the peer reported and label where it came from.
 */
function localOrPeer(local, peer, unit){
  if(has(local)) return local + (unit||'');
  if(has(peer))  return peer + (unit||'') + ' (peer)';
  return null;
}

const THR_STATE = ['idle','arming','running','done','failed'];
function renderThr(T){
  if(!T) return;
  rows('#tThr', [
    ['State',             THR_STATE[T.state] || T.state],
    ['Direction',         T.dir==0?'TX (this node sends)':'RX (this node receives)'],
    ['Protocol',          T.udp?'UDP':'TCP'],
    ['Average throughput',has(T.avg_kbps)&&T.avg_kbps? mbps(T.avg_kbps)+' Mbps' : null],
    ['Current throughput',T.state==2&&T.current_kbps? mbps(T.current_kbps)+' Mbps' : null],
    ['Bytes transferred', has(T.bytes)&&T.bytes? T.bytes.toLocaleString() : null],
    ['Duration',          T.duration_ms? (T.duration_ms/1000).toFixed(1)+' s' : null],
    ['Local endpoint',    T.local_addr? T.local_addr+':'+T.local_port : null],
    ['Remote endpoint',   T.remote_addr? T.remote_addr+':'+T.remote_port : null],
    ['Packets sent (UDP)',T.tx_frames],
    ['Packets received (UDP)', T.rx_frames],
    ['Lost datagrams (UDP)',   T.error_count],
    ['Out of order (UDP)',     T.out_of_sequence],
    ['Packet loss (UDP)', has(T.loss_pct)? T.loss_pct.toFixed(2)+' %' : null],
    ['Mean inter-packet gap', has(T.mean_ipg_ms)? T.mean_ipg_ms.toFixed(1)+' ms' : null],
    ['TCP retransmissions', null],
    ['Note',              T.note || null],
  ]);
}

function renderPing(P){
  if(!P) return;
  /* Do not fight the user: only sync the control when they are not using it. */
  const pe = $('#pEnable');
  if(document.activeElement !== pe) pe.checked = !!P.probing;
  rows('#tPing', [
    ['Peer',        P.peer],
    ['Probing',     P.probing?'on':'off'],
    ['RTT current', P.valid? P.last_ms.toFixed(1)+' ms' : null],
    ['RTT average', P.valid? P.avg_ms.toFixed(1)+' ms' : null],
    ['RTT min',     P.valid? P.min_ms.toFixed(1)+' ms' : null],
    ['RTT max',     P.valid? P.max_ms.toFixed(1)+' ms' : null],
    ['Probes sent', P.sent],
    ['Replies',     P.received],
    ['Lost',        P.lost],
    ['Loss',        has(P.loss_pct)? P.loss_pct.toFixed(2)+' %' : null],
  ]);
}

/* ---------- transport: SSE with polling fallback ---------- */
let pollTimer = null;
function startPolling(){
  if(pollTimer) return;
  pollTimer = setInterval(() => {
    fetch('/api/status').then(r=>r.json()).then(renderStatus).catch(()=>{});
  }, 1000);
}
function connect(){
  let es;
  try { es = new EventSource('http://' + location.hostname + ':81/'); }
  catch(e){ startPolling(); return; }
  es.onmessage = ev => {
    try { renderStatus(JSON.parse(ev.data)); } catch(e){}
    if(pollTimer){ clearInterval(pollTimer); pollTimer = null; }
  };
  es.onerror = () => { startPolling(); };
  setTimeout(() => { if(!statusCache) startPolling(); }, 3000);
}
fetch('/api/status').then(r=>r.json()).then(renderStatus).catch(()=>{});
connect();

/* ---------- charts ---------- */
function drawChart(id, pts, key, opts){
  opts = opts || {};
  const c = $(id); if(!c) return;
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth, h = parseInt(c.getAttribute('height'));
  c.width = w*dpr; c.height = h*dpr;
  const g = c.getContext('2d'); g.setTransform(dpr,0,0,dpr,0,0);
  g.clearRect(0,0,w,h);

  const padL = 42, padR = 8, padT = 8, padB = 18;
  const vals = pts.map(p => p[key]).filter(v => v !== null && v !== undefined);

  g.strokeStyle = '#2a3540'; g.lineWidth = 1;
  g.strokeRect(padL, padT, w-padL-padR, h-padT-padB);

  if(!vals.length){
    g.fillStyle = '#5d6b78'; g.font = '12px sans-serif'; g.textAlign = 'center';
    g.fillText('no data', w/2, h/2);
    return;
  }
  let min = Math.min.apply(null, vals), max = Math.max.apply(null, vals);
  if(opts.min !== undefined) min = Math.min(min, opts.min);
  if(opts.max !== undefined) max = Math.max(max, opts.max);
  if(max - min < (opts.span||1)){ const c0=(max+min)/2, s=(opts.span||1)/2; min=c0-s; max=c0+s; }
  const pad = (max-min)*0.1; min -= pad; max += pad;

  const X = i => padL + (w-padL-padR) * (pts.length<2?0.5:i/(pts.length-1));
  const Y = v => padT + (h-padT-padB) * (1 - (v-min)/(max-min));

  /* gridlines + labels */
  g.fillStyle = '#5d6b78'; g.font = '10px sans-serif'; g.textAlign = 'right';
  g.setLineDash([2,3]);
  for(let i=0;i<=3;i++){
    const v = min + (max-min)*i/3, y = Y(v);
    g.strokeStyle = '#222c35'; g.beginPath();
    g.moveTo(padL,y); g.lineTo(w-padR,y); g.stroke();
    g.fillText(v.toFixed(opts.dec===undefined?0:opts.dec), padL-4, y+3);
  }
  g.setLineDash([]);

  /* line, broken across gaps so missing data is visible as a gap */
  g.strokeStyle = opts.color || '#58a6ff'; g.lineWidth = 1.8;
  g.beginPath(); let drawing = false;
  pts.forEach((p,i) => {
    const v = p[key];
    if(v === null || v === undefined){ drawing = false; return; }
    if(!drawing){ g.moveTo(X(i), Y(v)); drawing = true; }
    else g.lineTo(X(i), Y(v));
  });
  g.stroke();
}

function loadHistory(){
  const sel  = $('#histWindow').value.split(':');
  const win  = +sel[0];
  const secs = +sel[1];
  /* The device trims to `secs` and decimates to `max`, so the response stays small. */
  fetch('/api/history?window='+win+'&secs='+secs+'&max=180').then(r=>r.json()).then(d => {
    const pts = d.points || [];
    drawChart('#cRssi', pts, 'rssi', {color:'#58a6ff', span:10});
    drawChart('#cThr',  pts.map(p=>({v: p.thr!==null? p.thr/1000 : null})), 'v',
              {color:'#3fb950', dec:2, min:0, span:1});
    drawChart('#cRtt',  pts, 'rtt',  {color:'#d29922', dec:1, min:0, span:5});
    drawChart('#cLoss', pts, 'loss', {color:'#f85149', dec:1, min:0, max:1, span:2});
    drawChart('#cMcs',  pts, 'mcs',  {color:'#bc8cff', min:0, max:10, span:2});
  }).catch(()=>{});
}
$('#btnRefreshHist').onclick = loadHistory;
$('#btnStatsReset').onclick = () => {
  if(confirm('Clear stored history and link counters?'))
    fetch('/api/stats/reset',{method:'POST'}).then(loadHistory);
};
$('#histWindow').onchange = loadHistory;
window.addEventListener('resize', () => {
  if($('#charts').classList.contains('active')) loadHistory();
});

/* ---------- tests ---------- */
$('#btnTestStart').onclick = () => {
  const b = new URLSearchParams({
    dir: $('#tDir').value, proto: $('#tProto').value,
    duration: $('#tDur').value, rate: $('#tRate').value, packet: $('#tPkt').value
  });
  fetch('/api/test/start', {method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:b})
    .then(r=>r.json()).then(j => { if(!j.ok) alert('Test failed: ' + (j.error||'unknown')); })
    .catch(e => alert('Request failed'));
};
$('#btnTestStop').onclick = () => fetch('/api/test/stop', {method:'POST'});

$('#btnPingApply').onclick = () => {
  const b = new URLSearchParams({
    enable: $('#pEnable').checked ? '1':'0', interval: $('#pInterval').value });
  fetch('/api/ping/config', {method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:b});
};
$('#btnPingReset').onclick = () => fetch('/api/ping/reset', {method:'POST'});

/* ---------- config ---------- */
let regions = null;
function loadConfig(){
  fetch('/api/config').then(r=>r.json()).then(c => {
    const f = $('#cfgForm');
    $$('input[name=role]').forEach(r => r.checked = (+r.value === c.role));
    for(const k in c){
      const el = f.elements[k];
      if(el && el.type !== 'radio') el.value = c[k];
    }
    if(!regions){
      fetch('/api/regions').then(r=>r.json()).then(d => {
        regions = d;
        const rs = $('#cfgRegion');
        rs.innerHTML = d.regions.map(r=>'<option'+(r===c.region?' selected':'')+'>'+r+'</option>').join('');
        rs.onchange = () => fillChannels(rs.value, null);
        fillChannels(c.region, c.channel);
      });
    } else fillChannels(c.region, c.channel);
  });
}
function fillChannels(region, sel){
  fetch('/api/channels?region='+encodeURIComponent(region)).then(r=>r.json()).then(d => {
    const cs = $('#cfgChannel');
    cs.innerHTML = d.channels.map(ch =>
      '<option value="'+ch.channel+'"'+(ch.channel==sel?' selected':'')+'>'+
      ch.channel+' — '+(ch.centre_freq_hz/1e6).toFixed(1)+' MHz, '+ch.bw_mhz+
      ' MHz, max '+ch.max_eirp_dbm+' dBm</option>').join('');
    const upd = () => {
      const ch = d.channels.find(x => x.channel == cs.value);
      $('#chanHint').textContent = ch ?
        ('Channel '+ch.channel+': centre '+(ch.centre_freq_hz/1e6).toFixed(1)+
         ' MHz, bandwidth '+ch.bw_mhz+' MHz, regulatory max '+ch.max_eirp_dbm+
         ' dBm EIRP, duty cycle '+(ch.duty_cycle_pct100/100).toFixed(2)+' %.') : '';
    };
    cs.onchange = upd; upd();
  });
}
function submitConfig(reboot){
  const fd = new FormData($('#cfgForm'));
  if(reboot) fd.append('reboot','1');
  fetch('/api/config', {method:'POST', body:new URLSearchParams(fd)})
    .then(r=>r.json()).then(j => {
      $('#cfgMsg').textContent = j.ok ?
        (reboot ? 'Saved. Rebooting…' : 'Saved. Radio changes apply after a reboot.')
        : ('Error: ' + (j.error||'unknown'));
    }).catch(() => { $('#cfgMsg').textContent = 'Request failed'; });
}
$('#cfgForm').onsubmit = e => { e.preventDefault(); submitConfig(false); };
$('#btnSaveReboot').onclick = () => submitConfig(true);
$('#btnReboot').onclick = () => {
  if(confirm('Reboot the device?')) fetch('/api/reboot', {method:'POST'});
};

/* ---------- logs ---------- */
function loadLog(){
  fetch('/api/log.txt').then(r=>r.text()).then(t => {
    const v = $('#logView'); v.textContent = t; v.scrollTop = v.scrollHeight;
  }).catch(()=>{});
}
$('#btnLogRefresh').onclick = loadLog;
$('#btnLogClear').onclick = () => fetch('/api/log/clear',{method:'POST'}).then(loadLog);
setInterval(() => {
  if($('#logs').classList.contains('active') && $('#logAuto').checked) loadLog();
}, 4000);
)JS";
