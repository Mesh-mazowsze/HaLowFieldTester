/*
 * web_server.h - management Wi-Fi AP, REST API and the SSE push channel.
 *
 * The panel is served from the 2.4 GHz management AP only; the HaLow link is
 * left entirely to the measurements.
 *
 * Live updates use Server-Sent Events on port 81 (a small dedicated socket
 * server) because the bundled WebServer class is synchronous and cannot hold
 * a streaming response open. The browser falls back to polling /api/status
 * once a second if EventSource is unavailable.
 */
#pragma once

#include <Arduino.h>

void webInit(void);     /* starts the management AP, HTTP server and SSE server */
void webTick(void);     /* pump both servers; call from loop() */

/* Builds the full status document used by /api/status and the SSE stream. */
void buildStatusJson(String &out);
