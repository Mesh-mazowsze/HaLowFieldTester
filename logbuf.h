/*
 * logbuf.h - in-RAM ring log, mirrored to Serial.
 *
 * The panel's "Logs" tab reads this; it is also downloadable as a text file.
 * Nothing is written to flash.
 */
#pragma once

#include <Arduino.h>

enum LogLevel : uint8_t {
  LOG_DEBUG = 0,
  LOG_INFO  = 1,
  LOG_WARN  = 2,
  LOG_ERROR = 3,
};

#define LOG_LINE_MAX   140
#define LOG_RING_LINES 160

void logInit(void);
void logPrintf(LogLevel lvl, const char *tag, const char *fmt, ...);
void logClear(void);

/* Appends the whole ring to `out` as plain text, oldest first. */
void logDumpText(String &out);
/* Appends the ring as a JSON array of {t,l,m}. */
void logDumpJson(String &out);
/* Monotonic counter, lets the UI fetch only what it has not seen. */
uint32_t logSeq(void);

#define LOGD(tag, ...) logPrintf(LOG_DEBUG, tag, __VA_ARGS__)
#define LOGI(tag, ...) logPrintf(LOG_INFO,  tag, __VA_ARGS__)
#define LOGW(tag, ...) logPrintf(LOG_WARN,  tag, __VA_ARGS__)
#define LOGE(tag, ...) logPrintf(LOG_ERROR, tag, __VA_ARGS__)
