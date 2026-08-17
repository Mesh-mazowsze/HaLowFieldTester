/*
 * HaLow Field Tester - Heltec HT-RC3268 (ESP32-S3 + HT-HC01 V2 / Morse Micro MM6108)
 *
 * One firmware image for both nodes of a two-node 802.11ah field test set.
 * The role (HaLow AP or HaLow STA) is chosen from the web panel and stored in
 * NVS, so the same binary is flashed to both boards.
 *
 *   phone/laptop --2.4 GHz mgmt Wi-Fi--> node A (HaLow AP)
 *                                          |  802.11ah, EU 868 MHz, 1 MHz
 *                                          v
 *                                        node B (HaLow STA)
 *
 * Target: heltec:esp_halow:HT-RC3268
 * The board links the HC01_V2_L (low band / 868 MHz) BCF, which is the EU part.
 *
 * See README.md for which diagnostics the MM6108 can and cannot provide.
 */

#include <Arduino.h>

#include "config.h"
#include "logbuf.h"
#include "halow_manager.h"
#include "link_monitor.h"
#include "rtt_test.h"
#include "peer_link.h"
#include "throughput_test.h"
#include "web_server.h"
#include "serial_cli.h"
#include "ui_screen.h"

/*
 * How long to wait for the HaLow link before starting the management AP.
 * Set to 0 to start the 2.4 GHz AP immediately.
 */
#ifndef HALOW_LINK_WAIT_MS
#define HALOW_LINK_WAIT_MS 45000
#endif

static bool s_serversStarted = false;

void setup() {
  Serial.begin(115200);
  delay(300);

  logInit();
  LOGI("BOOT", "HaLow Field Tester %s starting", FW_VERSION);
  /*
   * boards.txt links bcf_HC01_V2_L (the low-band / 868 MHz board config) for
   * HT-RC3268. Note that the board_desc string embedded in that BCF reads
   * "HC01_V2_H", which is a labelling error on Heltec's side - the L and H
   * archives are genuinely different files. So we report what is linked, and
   * do not claim the band from the BCF metadata.
   */
  LOGI("BOOT", "board HT-RC3268, ESP32-S3, HT-HC01 V2, linked BCF: bcf_HC01_V2_L");
  LOGI("BOOT", "reset reason %d, free heap %lu B",
       (int)esp_reset_reason(), (unsigned long)ESP.getFreeHeap());

  cfgLoad();
  LOGI("BOOT", "role: HaLow %s", roleName(g_cfg.role));

  /*
   * Order matters, and it is not negotiable: the HaLow link must be up before
   * the ESP32's 2.4 GHz Wi-Fi is started. Every Heltec example blocks on
   * HaLow.status() != WL_CONNECTED and only then calls WiFi.begin() /
   * WiFi.softAP(). Starting the 2.4 GHz radio while the MM6108 is still
   * scanning prevents the STA from ever finding the AP.
   *
   * The timeout keeps the tester usable in the field: if the link never comes
   * up, the panel still appears so you can diagnose it from a phone.
   */
  halowInit();
  if (!halowStart()) {
    LOGE("BOOT", "HaLow start failed - panel will still be available");
  } else if (HALOW_LINK_WAIT_MS > 0) {
    halowWaitForLink(HALOW_LINK_WAIT_MS);
  }

  webInit();

  /* Sockets after both stacks exist. */
  rttInit();
  peerInit();
  thrInit();
  thrRegisterPeerHandler();

  linkMonitorInit();

  if (g_cfg.contEnabled) {
    rttSetProbing(true, g_cfg.contIntervalMs);
  }

  cliInit();

  /* Optional RS-T108 panel; the node runs headless when none is fitted. */
  uiInit();

  LOGI("BOOT", "ready, free heap %lu B", (unsigned long)ESP.getFreeHeap());
}

void loop() {
  halowTick();
  linkMonitorTick();
  rttTick();
  peerTick();
  thrTick();
  webTick();
  cliTick();
  uiTick();

  /* iperf servers need the link, so start them once it is up. */
  if (!s_serversStarted && halowIsUp()) {
    s_serversStarted = true;
    thrStartServers();
  }

  delay(2);
}
