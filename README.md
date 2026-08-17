# HaLow Field Tester — Heltec HT-RC3268 / MM6108 / EU 868 MHz

A two-node Wi‑Fi HaLow (802.11ah) field test set built on the Heltec **HT‑RC3268**
(ESP32‑S3 + **HT‑HC01 V2** module, Morse Micro **MM6108**), for the **EU / 868 MHz,
1 MHz bandwidth** variant.

One firmware image is flashed to both boards. The role — HaLow **AP** or HaLow
**STA** — is chosen in the web panel and stored in NVS, so it survives a reboot.

```
   phone / laptop
         │  2.4 GHz management Wi-Fi (this firmware's own AP)
         ▼
   HT-RC3268  #1  ── HaLow AP
         │
         │  802.11ah · EU · 868 MHz · 1 MHz BW
         ▼
   HT-RC3268  #2  ── HaLow STA
```

Leave node #1 on a mast, take node #2 into the field, and watch the link
degrade live on your phone — no laptop, no serial monitor.

---

## 1. Framework decision: Arduino / ESP_HaLow (not ESP‑IDF)

**Decision: use Heltec's Arduino `ESP_HaLow` platform.**

The reason is that this platform is not merely an Arduino wrapper — it ships the
**complete Morse Micro MM‑IoT‑SDK 2.10.4** headers, and the prebuilt static
libraries that back them, inside the `wifi-halow` library. That means the
low‑level diagnostic API is reachable *directly from an Arduino sketch* with a
plain `#include "mmwlan.h"`.

I verified this rather than assuming it. Running `xtensa-esp32s3-elf-nm
--defined-only` over the eight archives in
`libraries/wifi-halow/src/esp32s3/` confirms these symbols are really present
and linkable (not just declared in a header):

| Symbol | Provides | Status |
|---|---|---|
| `mmwlan_get_rssi` | RSSI | ✅ `libmorse.a` |
| `mmwlan_get_rc_stats` / `mmwlan_free_rc_stats` | MCS, bandwidth, GI, per‑rate success | ✅ |
| `mmwlan_get_umac_stats` / `mmwlan_clear_umac_stats` | queue drops, CCMP failures, reorder stats | ✅ |
| `mmwlan_get_version` | MM6108 firmware version, chip ID, morselib version | ✅ |
| `mmwlan_get_bcf_metadata` | BCF version, board description, build string | ✅ |
| `mmwlan_get_duty_cycle_stats` | duty cycle configuration/state | ✅ |
| `mmwlan_get_bssid` / `mmwlan_ap_get_bssid` | peer / own BSSID | ✅ |
| `mmwlan_get_vif_mac_addr` | HaLow MAC per VIF | ✅ |
| `mmwlan_override_max_tx_power` | TX power ceiling | ✅ |
| `mmwlan_ap_get_sta_status` | per‑station state + AID | ✅ |
| `mmiperf_start_{tcp,udp}_{client,server}` | throughput tests | ✅ `libmmiperf.a` |
| `mmiperf_get_interim_report` | live throughput | ✅ |
| **`mmwlan_get_aid`** | STA AID | ❌ **declared in `mmwlan.h:1404` but defined in no archive** — calling it fails at *link* time. Not used by this firmware. |

Since every diagnostic we need is available, dropping to ESP‑IDF +
`morsemicro/halow` would only cost us Heltec's already‑correct HT‑RC3268 board
bring‑up: the HC01 SPI pin map, the LDO enable on GPIO3, and — importantly —
the **band‑correct BCF**. `boards.txt` pins `HT-RC3268.build.bcf_lib=bcf_HC01_V2_L`,
i.e. the **low‑band (868 MHz) board configuration**, which is exactly the EU
part. That is worth keeping.

---

## 2. Which diagnostics are actually available

This was the central research question, and the answer is enforced in the code:
**anything not truly readable is reported as `null` / `—`, never estimated.**

### ✅ Available and shown

| Metric | Source |
|---|---|
| **RSSI** | `mmwlan_get_rssi()` — returns `INT32_MIN` on error, which we treat as "unavailable" |
| **MCS** | `mmwlan_get_rc_stats()`, decoding the `rate_info` bitfield |
| **Bandwidth, guard interval** | same bitfield (`BW` bits 0–3, `Guard` bit 8) |
| **PHY rate** | looked up from the *measured* MCS/BW/GI in the standard 802.11ah rate table (`mcs_table.cpp`). **Not** inferred from RSSI. |
| **PHY error rate (PER)** | `total_sent` vs `total_success` deltas across the rate table |
| **Frames attempted / succeeded** | cumulative rate‑table totals |
| **Link state, association uptime, disconnect count** | HaLow events + `HaLow.status()`, timed locally |
| **Peer / AP MAC (BSSID)** | `mmwlan_get_bssid()` (STA), peer telemetry (AP) |
| **Own HaLow MAC** | `mmwlan_get_vif_mac_addr()` |
| **MM6108 firmware version, chip ID** | `mmwlan_get_version()` |
| **BCF version / board / build** | `mmwlan_get_bcf_metadata()` — but see the labelling caveat in §4 |
| **Morse SDK version** | `MM_VERSION_BUILDID` (2.10.4‑esp32) |
| **Regulatory domain, channel → centre frequency, bandwidth, max EIRP, duty cycle** | the library's own `mmwlan_regdb.h` tables — not hard‑coded here |
| **UMAC counters** | `mmwlan_get_umac_stats()`: TX/RX queue drops, CCMP failures, RX alloc failures, reorder timeouts, HW restarts |
| **Duty cycle target/mode** | `mmwlan_get_duty_cycle_stats()` |
| **Throughput (TCP/UDP, both directions)** | `mmiperf` |
| **UDP datagrams sent/received/lost/out‑of‑order** | `mmiperf_report` |
| **Mean inter‑packet gap (UDP)** | `ipg_sum_ms / ipg_count` — reported honestly as mean IPG, **not** relabelled "jitter" |
| **RTT, RTT min/avg/max, probe loss** | our own UDP echo protocol |
| ESP32 uptime, free heap, PSRAM, CPU temperature, Wi‑Fi MAC, reset reason | ESP32 API |

### ❌ Not available — shown as `—` / exported empty, never invented

| Metric | Why |
|---|---|
| **SNR** | No live noise measurement exists in the connected state, so SNR cannot be computed. |
| **Noise floor** | `noise_dbm` exists **only** in `mmwlan_scan_result`, i.e. during a scan — not while associated. |
| **Actual TX power** | The API has no "read current TX power". Only `mmwlan_override_max_tx_power()` (write), and it can only *lower* the regulatory ceiling, never raise it. The panel therefore shows the **configured ceiling** (your override, or the channel's regulatory max), explicitly labelled. |
| **TX/RX packet & byte counters** | `mmipal_get_link_packet_counts()` exists, but it requires `LWIP_STATS`, which **is not defined** in this platform's `halow_config.h`. It would silently return 0, so it is not used. |
| **Retry count** | Only per‑rate attempts vs successes are exposed, not a raw retry counter. PER is reported instead. |
| **Disconnect reason code** | The Arduino event layer delivers `ARDUINO_HALOW_EVENT_STA_DISCONNECTED` with no reason field. |
| **Channel utilisation** | Not exposed by the API. (Duty‑cycle stats are reported, which is a different quantity.) |
| **Per‑station RSSI on the AP** | `mmwlan_ap_get_sta_status()` returns only state/AID/MAC, and Heltec's `HalowAPClass::getStationList()` is a **stub returning 0**. See §3. |
| **TCP retransmissions** | Not in `mmiperf_report`. |
| **Beacon interval / capability / TSF** | Present in scan results only, not while associated. |
| **Simultaneous bidirectional UDP** | `mmiperf` has no dual‑test mode. |

---

## 3. Why there is a peer telemetry link

Two API facts drive the architecture:

1. `mmwlan_get_rssi()` measures the signal **received from the AP** — it is a
   STA‑side measurement and is meaningless on the AP node.
2. The AP cannot enumerate its associated stations: `getStationList()` is a stub.

So each node sends its own measurements to the other once per second over a
small UDP protocol (`peer_link.cpp`, default port 5556). That is what lets you
connect a phone to the **AP** node and still see the **remote STA's** RSSI and
MCS. Values sourced from the peer are labelled as such in the UI
("RSSI source: reported by peer node").

The same channel arms the far side of a throughput test, so one tap sets up both ends.

---

## 4. EU / 868 MHz channel plan

Taken verbatim from the library's regulatory database
(`wifi-halow/src/mmwlan_regdb.h`, `s1g_channels_EU[]`) — the firmware reads
this table at runtime rather than hard‑coding it. Region string is **`"EU"`**.

| Channel | Centre frequency | BW | Max EIRP | Duty cycle | S1G op class |
|---|---|---|---|---|---|
| **1** | 863.5 MHz | 1 MHz | 16 dBm | 2.80 % | 6 |
| **3** | 864.5 MHz | 1 MHz | 16 dBm | 2.80 % | 6 |
| **5** | 865.5 MHz | 1 MHz | 16 dBm | 2.80 % | 6 |
| **7** | 866.5 MHz | 1 MHz | 16 dBm | 2.80 % | 6 |
| **9** | 867.5 MHz | 1 MHz | 16 dBm | 2.80 % | 6 |
| 2 | 864.0 MHz | 2 MHz | 16 dBm | 2.80 % | 7 |
| 6 | 866.0 MHz | 2 MHz | 16 dBm | 2.80 % | 7 |

Occupied band: 863.0–868.0 MHz. **Default in this firmware: region `EU`,
channel 5 (865.5 MHz), 1 MHz.** No US/915 MHz default is ever applied.

> **BCF labelling caveat (cosmetic).** `boards.txt` links `bcf_HC01_V2_L` — the
> low-band (868 MHz) board configuration — for HT‑RC3268, yet the running
> device reports its BCF board description as **`HC01_V2_H`**. The archives
> `libbcf_HC01_V2_L.a` (1064 B) and `libbcf_HC01_V2_H.a` (1042 B) are genuinely
> different files, but **both embed the same `board_desc` string**. The module
> itself is laser-etched **868**, and 868 MHz operation is confirmed working,
> so this is only a wrong string inside Heltec's low-band BCF — not a wrong
> BCF. Practical consequence: **do not use the BCF board description to infer
> the band**; trust `build.bcf_lib` in `boards.txt`.

> **Note on a Heltec fallback.** `HalowClass::AP()` silently falls back to the
> *last* channel in the region's list if the requested channel is not found —
> for EU that is channel 6, which is a **2 MHz** channel. This firmware
> validates the channel against the regulatory table *before* calling `AP()`
> and refuses to start otherwise, so you can never end up on 2 MHz by accident.

> ⚠️ **TX power and channel configuration must comply with local regulations.**
> The 868 MHz band is duty‑cycle limited. The UI does not hard‑lock the
> parameters — it is also a lab tool — so lawful operation is your responsibility.

---

## 4a. ⚠️ The EU 1 MHz beacon problem — and the fix

**This is the single most important finding in this project, and it is why
Heltec's own AP example cannot be joined on any EU 1 MHz channel.**

### Symptom

On any EU 1 MHz channel (1, 3, 5, 7, 9) the AP starts and reports itself
healthy — `mmwlan_ap_enable()` returns `MMWLAN_SUCCESS`, `mmwlan_ap_get_bssid()`
returns a valid BSSID — but **no station can ever find it**. A STA scan across
all EU channels at 2 s dwell returns *0 networks*, forever. On the 2 MHz
channel (6, 866.0 MHz) the identical setup associates in ~3 seconds.

This reproduces with Heltec's **stock, unmodified** `HalowAP.ino` +
`HalowClientStaticIP.ino`, so it is not a bug in this firmware.

### Cause

EU 868 MHz is duty-cycle limited to **2.80 %**. A beacon is sent at the lowest
MCS, so its airtime depends on the channel bandwidth:

| Bandwidth | MCS0 rate | Beacon airtime | Duty at 100 TU default | Fits in 2.80 %? |
|---|---|---|---|---|
| **1 MHz** | 300 kbps | ~4–5 ms | **~4–5 %** | ❌ **no** |
| 2 MHz | 650 kbps | ~2 ms | ~2.2 % | ✅ yes |

At the default beacon interval of `MMWLAN_DEFAULT_AP_BEACON_INTERVAL_TUS`
(100 TU = 102.4 ms), 1 MHz beaconing alone would exceed the EU duty-cycle
allowance. The driver therefore suppresses the beacons, and the AP is
effectively invisible. Confirmed on hardware: an AP left on EU ch5 for 55 s had
**0 frames transmitted** in its rate-control table.

Heltec's `HalowClass::AP()` always passes `beacon_interval_tus = 0` (→ the
100 TU default) and gives no way to change it.

### Fix

This firmware does **not** use `HaLow.AP()`. It builds `struct mmwlan_ap_args`
itself and calls `mmwlan_ap_enable()` directly, so it can set both the beacon
interval and the primary bandwidth explicitly (`halow_manager.cpp`,
`startApDirect()`):

```c
args.pri_bw_mhz          = ci.bwMhz;   /* explicit, not Heltec's 0 = "auto" */
args.beacon_interval_tus = beaconTus;  /* 300 TU on duty-cycle-limited 1 MHz */
```

`beaconIntervalTus = 0` (the default) selects automatically:

* duty cycle ≥ 100 % → **100 TU** (the standard default)
* duty-cycle limited, 1 MHz → **300 TU** (307 ms)
* duty-cycle limited, wider → **200 TU**

It is overridable from the panel (**Config → AP beacon interval**) and the
serial console (`beacon <TU>`) for experimentation.

**Result:** EU ch5 / 1 MHz associates and stays up.

### The second half of the problem: scan dwell

Fixing the beacon interval makes association *possible*, but not yet *reliable* —
measured **1 in 3** attempts even with a healthy AP.

The cause is the other side of the same coin. The driver's internal connect
scan dwells for `MMWLAN_SCAN_DEFAULT_DWELL_TIME_MS` — **30 ms per channel**.
With a 307 ms beacon period, a 30 ms listening window catches a beacon roughly
one time in ten, so the STA has to rely on repeated scan passes with
exponential backoff.

So this firmware also widens the dwell to longer than one beacon period
(`halow_manager.cpp`, STA branch):

```c
struct mmwlan_scan_config sc = MMWLAN_SCAN_CONFIG_INIT;
sc.dwell_time_ms = beaconPeriodMs + 120;   /* ~427 ms for a 300 TU beacon */
mmwlan_set_scan_config(&sc);
```

Measured over 10 reboots: association went from **1/3 to 9/10**, typically in
**~7 seconds**. The cost is a slower scan, which is irrelevant here since the
STA only scans while joining.

**Takeaway:** on a duty-cycle-limited narrow channel the beacon interval and
the scan dwell must be tuned *together*. Changing one without the other leaves
you with a link that either never forms or forms unpredictably.

### What this means for expected performance

The same 2.80 % limit caps throughput. Measured on EU ch5 / 1 MHz with the two
nodes on a bench:

```
PHY rate (MCS7, 1 MHz, SGI):  3.33 Mbps
TCP throughput:                 88 kbps
UDP throughput:                 82 kbps   (0.00 % loss)
RTT:                            28 ms min, high variance
```

88 kbps ≈ 3 Mbps × 2.80 % — the link is running **at the regulatory ceiling**,
not underperforming. Expect roughly **80–90 kbps** on EU 868 MHz at 1 MHz, and
latency in the tens-to-hundreds of ms because transmissions are spread to
respect the duty cycle. If you need more, EU channel 6 (2 MHz) roughly doubles
it, at the cost of occupied bandwidth.

---

## 5. Build and flash

### Required

* **Arduino IDE 2.x** (or `arduino-cli` ≥ 1.x)
* **Heltec ESP_HaLow** platform **3.0.0** — install via Boards Manager using
  this Additional Boards Manager URL:
  `https://resource.heltec.cn/download/package_heltec_esp_halow_index.json`
* No external libraries. Everything (web UI included) is in this sketch.

### Board settings

| Setting | Value |
|---|---|
| Board | **HT-RC3268** (`heltec:esp_halow:HT-RC3268`) |
| PSRAM | OPI PSRAM |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |
| Partition scheme | *fixed by the variant* — no menu; see note below |

The variant's `partitions.csv` gives 3.5 MB app / 640 KB SPIFFS / NVS. There is
no flash‑size or partition menu for this board; nothing needs changing.

### Command line

```bash
arduino-cli compile -b heltec:esp_halow:HT-RC3268 HaLowFieldTester
```

```bash
arduino-cli upload -b heltec:esp_halow:HT-RC3268 -p COM7 HaLowFieldTester
```

Current build size: **~2.04 MB flash (55 %)**, **~123 KB RAM (37 %)**.

**No filesystem upload is needed.** The web UI lives in PROGMEM, so flashing the
firmware is a single step — deliberate, because a data‑partition upload step is
awkward in Arduino IDE 2.x.

---

## 6. First run — setting up the two nodes

Flash the **same binary** to both boards. Then, for each one:

1. Power it up. It creates a 2.4 GHz management AP:
   * SSID `HaLow-Tester-XXXX` (XXXX = last two MAC bytes)
   * Password `halowtester`
   * Panel at **http://192.168.4.1/**
2. Connect a phone/laptop to that SSID and open the panel.
3. Go to **Config**.

**Node A — the AP (mast side):**

| Field | Value |
|---|---|
| Role | HaLow **AP** |
| HaLow SSID | `HaLow-Test` |
| Passphrase | `halow12345` (SAE) |
| Region / Channel | `EU` / `5 — 865.5 MHz` |
| IP | `192.168.50.1` |
| Netmask | `255.255.255.0` |
| Gateway | `192.168.50.1` |
| Peer IP | `192.168.50.2` |

**Node B — the STA (roving side):**

| Field | Value |
|---|---|
| Role | HaLow **STA** |
| HaLow SSID / passphrase | *same as node A* |
| Region / Channel | *same as node A* |
| IP | `192.168.50.2` |
| Gateway | `192.168.50.1` |
| Peer IP | `192.168.50.1` |

Press **Save & reboot** on each. Addresses are static because **the HaLow AP in
this framework has no DHCP server** (stated explicitly in Heltec's own
`HalowAP.ino`).

Give both nodes ~15 s. The header pill should read **LINK UP**, and the
**Peer node** card should show `(live)`.

### An example test

1. Open the panel of whichever node you have with you.
2. **Tests → Ping/RTT**: tick *Continuous probe*, interval 1000 ms, **Apply**.
   The dashboard now shows live RSSI, MCS, RTT and loss, and the charts fill in.
3. **Tests → Throughput**: Protocol `TCP`, Direction `TX`, Duration `10 s`,
   **Start test**. Results appear when it completes.
4. Repeat with `UDP` and a target of e.g. 2000 kbps.
5. Walk or drive away, watching the dashboard.
6. **Tests → Export**: **Download CSV** or **JSON**.

---

## 7. Web panel

* **Dashboard** — live link metrics, peer node, HaLow configuration, full link
  statistics, device, radio/firmware versions, regulatory notice.
* **Charts** — RSSI, throughput, RTT, packet loss and MCS over 5 / 15 / 60 min.
  RAM only (~36 KB of ring buffers); nothing is written to flash. Gaps in the
  data are drawn as gaps, not interpolated.
* **Tests** — throughput tester, ping/RTT, CSV/JSON export.
* **Config** — role, radio, addressing, management Wi‑Fi, ports.
* **Logs** — boot, HC01 init, MM6108/BCF versions, association, disconnects,
  errors, test start/stop. Downloadable, clearable, mirrored to Serial @115200.

### Reaching both panels

Each node runs its **own, independent** management Wi‑Fi, so both legitimately
use `192.168.4.1` — they are separate networks and never meet, exactly like two
home routers both being `192.168.1.1`. You tell them apart by SSID.

Each node also serves the same panel on its **HaLow** address, and NAPT is
enabled on the management AP, so from one node's Wi‑Fi you can open the other
node's full panel:

| Connected to | Local panel | Remote node's panel |
|---|---|---|
| `HaLow-Tester-XXXX` (AP node) | http://192.168.4.1/ | http://192.168.50.2/ |
| `HaLow-Tester-YYYY` (STA node) | http://192.168.4.1/ | http://192.168.50.1/ |

Verified device-to-device over the HaLow link (`HTTP/1.1 200 OK`, ~2.4 KB, in
0.8 s AP→STA and 3.6 s STA→AP — slow because of the duty cycle, but working).
The phone-to-remote-panel path uses the same route plus NAPT; the NAPT hop
itself has not been exercised from a real Wi‑Fi client here, as this
development machine has no Wi‑Fi adapter.

Live updates arrive by **Server‑Sent Events on port 81**. The bundled
`WebServer` class is synchronous and cannot hold a streaming response open, so
SSE runs on its own small socket server; the page falls back to polling
`/api/status` at 1 Hz if `EventSource` fails.

---

## 7a. Serial console

A line-based console on the USB serial port (115200), as a fallback when you
cannot reach the panel — and the quickest way to script a two-node bring-up.
Type `help` for the list.

```
role ap|sta            region <XX>          chan <n>
ssid <s>  pass <s>     ip|mask|gw|peer <a>  txpower <dBm>
beacon <TU>            save  reboot  factory
status   cfg   chans   state   scan [ms]
ping on|off [ms]       test tcp|udp tx|rx [s] [kbps]   stop
json [history]         httpget [ip]
```

`json` prints the exact document the web panel consumes, so the API can be
validated without a Wi‑Fi client attached — that is how the field population of
every dashboard value was verified here. `httpget [ip]` fetches `/api/status`
from the peer over HaLow, which distinguishes "the panel is broken" from "the
link is down".

`state` and `scan` are diagnostics: `state` dumps the raw driver view (AP BSSID,
`mmwlan_get_sta_state()`, duty cycle), and `scan [dwell_ms]` runs a HaLow scan
and lists what the radio can actually hear — the fastest way to tell "the AP is
not transmitting" from "the STA cannot associate".

---

## 8. REST API

All responses are JSON unless noted. Unavailable values are `null`.

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status` | Full status document (device, halow, link, peer, rtt, throughput, radio) |
| GET | `/api/halow` | HaLow configuration (currently the full document) |
| GET | `/api/stats` | Link statistics (currently the full document) |
| GET | `/api/history?window=0\|1&max=N` | History; window 0 = 15 min @1 s, 1 = 60 min @6 s |
| GET | `/api/regions` | Regulatory domains in the library database |
| GET | `/api/channels?region=EU` | Channel table with centre frequency, BW, max EIRP, duty cycle |
| GET | `/api/config` | Current configuration |
| POST | `/api/config` | Update configuration (form‑encoded; add `reboot=1` to restart) |
| POST | `/api/test/start` | `dir=tx\|rx`, `proto=tcp\|udp`, `duration`, `rate`, `packet` |
| POST | `/api/test/stop` | Stop tracking the current test (see limitation below) |
| POST | `/api/ping/config` | `enable=0\|1`, `interval` (ms) |
| POST | `/api/ping/reset` | Reset RTT statistics |
| POST | `/api/reboot` | Reboot |
| GET | `/api/log.txt`, `/api/log.json` | Log |
| POST | `/api/log/clear` | Clear log |
| GET | `/api/export.csv?window=0` | CSV export |
| GET | `/api/export.json?window=0` | JSON export (status + history) |

### CSV schema

```
timestamp_ms,rssi_dbm,snr_db,noise_dbm,mcs,bw_mhz,sgi,phy_rate_kbps,
rtt_ms,loss_pct,phy_per_pct,throughput_kbps,link_up,tx_power_dbm,
tx_packets,rx_packets
```

`snr_db`, `noise_dbm`, `tx_power_dbm`, `tx_packets` and `rx_packets` are always
**empty** — they are in the schema so the column layout stays stable, but the
MM6108 API cannot supply them (§2). They are never fabricated.

---

## 9. Known limitations

1. **`mmiperf` cannot abort a running session.** There is no `mmiperf_stop*` in
   the API. The firmware works around this by keeping *persistent* TCP and UDP
   iperf servers and only ever starting bounded *clients*, so every test ends on
   its own. **Stop** therefore stops *tracking* the test locally; the underlying
   session still runs to its configured duration. This is stated in the UI note.
2. **No simultaneous bidirectional throughput test** — `mmiperf` has no dual mode.
3. **RSSI is a STA‑side measurement.** On the AP node it comes from peer
   telemetry and is labelled accordingly. If the peer link is down, the AP shows
   `—` rather than a stale number.
4. **MCS/PHY require traffic.** The rate table only moves when frames are sent.
   With no traffic for 15 s the reading is marked stale and reported as
   unavailable. Enabling the continuous probe keeps it fresh.
5. **PHY rate is a table lookup**, from the measured MCS/BW/GI — it is the
   nominal PHY rate, not a measured goodput. Use the throughput tester for goodput.
6. **Static IP only on HaLow** — no DHCP server in the HaLow AP.
7. **Changing role/radio settings requires a reboot.**
8. **History is RAM‑only** and lost on reboot — export before power‑cycling.
9. **Max 2 concurrent SSE clients**, because `CONFIG_LWIP_MAX_SOCKETS` is 10 on
   this platform and the sockets must be shared with iperf.
10. **GPS is not implemented** (not required for v1) — see §11.
11. **Throughput is duty-cycle bound on EU 1 MHz** — expect ~80–90 kbps, not
    the 3 Mbps PHY rate. See §4a.
12. **Two library landmines this firmware works around** (both hit on real
    hardware; worth knowing if you write your own sketch):
    * `HalowSTAClass::begin()` feeds the passphrase straight into
      `mmosal_safer_strcpy()` with **no NULL check** (`HalowSTA.cpp:147`), so
      `HaLow.begin(ssid, NULL, …)` for an open network dereferences NULL and
      puts the device into a **panic boot loop**. `HaLow.AP()` guards against
      this; `begin()` does not. Always pass `""`, never `NULL`.
    * This firmware starts the ESP32's 2.4 GHz Wi-Fi only after the HaLow link
      is up (45 s timeout), matching every Heltec example, which blocks on
      `HaLow.status() != WL_CONNECTED` before calling `WiFi.softAP()` /
      `WiFi.begin()`. **In fairness: I measured this and found no difference** —
      1/3 association either way, before the scan-dwell fix. It is kept because
      it follows the vendor's documented pattern and, now that association
      takes ~7 s, it delays the panel by only a few seconds. Do not treat it as
      a proven requirement.

### Verified on hardware

Two HT‑RC3268 boards, EU / 868 MHz / channel 5 / **1 MHz**, SAE:

* Same image on both; roles set from NVS and persisted across reboots
* **Association in ~7 s, 9 out of 10 reboots** (was 1 in 3 before the scan-dwell fix)
* `MM6108A2`, MM firmware `1.17.6`, BCF `v12.1.0` read live from the transceiver
* Live RSSI, and **MCS7 / 1 MHz / SGI → 3.33 Mbps** decoded from the rate table
* RTT via UDP echo (28 ms min), 0 % probe loss, peer telemetry live in both directions
* TCP 88 kbps / UDP 82 kbps with 0.00 % datagram loss; iperf servers on both ends

---

## 10. Troubleshooting

| Symptom | Check |
|---|---|
| No `HaLow-Tester-XXXX` SSID | Serial @115200. Panel starts even if HaLow fails, so a missing SSID means an early boot fault. |
| **LINK DOWN** forever on an EU 1 MHz channel, STA `scan` finds 0 networks | The duty-cycle/beacon problem — see §4a. Check the AP boot log for the `beacon NNN TU` line; on EU 1 MHz it must be ~300 TU. If you set a manual `beacon` value that is too short the AP stops beaconing entirely: set `beacon 0` (auto). |
| **LINK DOWN** forever, other cases | Both nodes on the same region *and* channel? Same SSID/passphrase? One AP and one STA, not two of the same? |
| Device reboots in a loop right after the `BCF …` log line | A NULL passphrase reached `HaLow.begin()`. Fixed here; in your own sketch pass `""`, never `NULL`. |
| Very high PHY error rate with both boards on one desk | Receiver overload: at 16 dBm and ~20 cm apart the receiver sees about −2 dBm. Lower `txpower` or separate the boards — it is not a link fault. |
| Link up, but **Peer node** shows `(no data)` | Peer IP wrong, or the two nodes are on different subnets. Both must be in the same /24. |
| RSSI shows `—` on the AP node | Expected: RSSI is STA‑side. It appears once peer telemetry is live. |
| MCS/PHY show `—` | No traffic. Enable the continuous probe. |
| Throughput test says "could not reach peer" | Peer telemetry is down — fix the peer link first. |
| Throughput 0 / test times out | Confirm both nodes are up; the receiving side needs its iperf servers, which start on link‑up. |
| `mmwlan_get_version()` failed in the log | The MM6108 did not boot — check the LDO on GPIO3 and that the board really is an HT‑RC3268. |
| Upload fails | Hold BOOT while plugging in; try 115200. |

---

## 11. Extending it later (GPS)

The sample record (`struct Sample` in `stats.h`) and the CSV/JSON exporters are
the only places that need touching:

1. Add `latitude`, `longitude`, `distance_from_peer` to `struct Sample`.
2. Fill them in `linkMonitorTick()` from a GPS module (a UART is free).
3. Add the columns in `histCsvHeader()` / `histCsvRows()` and the fields in
   `histJson()`.
4. Peer position can ride along in `PeerTelemetryMsg` (bump `PEER_VERSION`),
   which then gives `distance_from_peer` directly.

Everything already emits `null`/empty for absent data, so partial GPS coverage
degrades cleanly.

---

## 12. Source layout

```
HaLowFieldTester/
  HaLowFieldTester.ino   setup()/loop() only
  config.{h,cpp}         NVS-backed configuration, role selection
  logbuf.{h,cpp}         RAM ring log, mirrored to Serial
  halow_manager.{h,cpp}  radio bring-up, versions, regulatory/channel tables
  link_monitor.{h,cpp}   per-second sampling: RSSI, rate table, UMAC, duty cycle
  mcs_table.{h,cpp}      802.11ah MCS/BW/GI -> PHY rate
  stats.{h,cpp}          history ring buffers, CSV/JSON export
  rtt_test.{h,cpp}       UDP echo responder + prober (also the continuous test)
  peer_link.{h,cpp}      node-to-node telemetry and remote test arming
  throughput_test.{h,cpp} mmiperf wrapper
  web_server.{h,cpp}     management AP, REST API, SSE
  web_assets.{h,cpp}     HTML/CSS/JS in PROGMEM
  README.md
```

Arduino requires the sketch's own files to sit next to the `.ino`, so there is
no `src/` subdirectory; the module split requested is preserved otherwise. The
web assets are compiled in rather than served from `data/`, so that flashing
stays a single step.
