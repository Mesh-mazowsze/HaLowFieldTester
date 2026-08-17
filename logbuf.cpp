#include "logbuf.h"
#include <stdarg.h>

struct LogEntry {
  uint32_t ms;
  uint8_t  lvl;
  char     msg[LOG_LINE_MAX];
};

static LogEntry     s_ring[LOG_RING_LINES];
static uint16_t     s_head  = 0;   /* next slot to write */
static uint16_t     s_count = 0;
static uint32_t     s_seq   = 0;
static portMUX_TYPE s_mux   = portMUX_INITIALIZER_UNLOCKED;

static const char *levelStr(uint8_t l) {
  switch (l) {
    case LOG_DEBUG: return "DBG";
    case LOG_INFO:  return "INF";
    case LOG_WARN:  return "WRN";
    default:        return "ERR";
  }
}

void logInit(void) {
  portENTER_CRITICAL(&s_mux);
  s_head = s_count = 0;
  s_seq  = 0;
  portEXIT_CRITICAL(&s_mux);
}

void logClear(void) {
  portENTER_CRITICAL(&s_mux);
  s_head = s_count = 0;
  portEXIT_CRITICAL(&s_mux);
  logPrintf(LOG_INFO, "LOG", "log cleared");
}

uint32_t logSeq(void) {
  return s_seq;
}

void logPrintf(LogLevel lvl, const char *tag, const char *fmt, ...) {
  char body[LOG_LINE_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  uint32_t now = millis();

  /* Room for the "[tag] " prefix on top of a full-length body. */
  char line[LOG_LINE_MAX + 32];
  snprintf(line, sizeof(line), "[%s] %s", tag, body);

  /* Serial mirror, with a readable timestamp. */
  Serial.printf("%7lu.%03lu %s %s\r\n", (unsigned long)(now / 1000),
                (unsigned long)(now % 1000), levelStr(lvl), line);

  portENTER_CRITICAL(&s_mux);
  LogEntry *e = &s_ring[s_head];
  e->ms  = now;
  e->lvl = (uint8_t)lvl;
  strlcpy(e->msg, line, LOG_LINE_MAX);
  s_head = (uint16_t)((s_head + 1) % LOG_RING_LINES);
  if (s_count < LOG_RING_LINES) s_count++;
  s_seq++;
  portEXIT_CRITICAL(&s_mux);
}

/* Index of the oldest entry. Caller must hold no lock; we snapshot under lock. */
static uint16_t oldestIndex(void) {
  return (uint16_t)((s_head + LOG_RING_LINES - s_count) % LOG_RING_LINES);
}

void logDumpText(String &out) {
  uint16_t count = s_count;
  uint16_t idx   = oldestIndex();
  char     ts[24];

  out.reserve(out.length() + (size_t)count * 64);
  for (uint16_t i = 0; i < count; i++) {
    const LogEntry &e = s_ring[(idx + i) % LOG_RING_LINES];
    snprintf(ts, sizeof(ts), "%7lu.%03lu %s ", (unsigned long)(e.ms / 1000),
             (unsigned long)(e.ms % 1000), levelStr(e.lvl));
    out += ts;
    out += e.msg;
    out += "\n";
  }
}

static void appendJsonEscaped(String &out, const char *s) {
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)c);
      out += buf;
    } else {
      out += c;
    }
  }
}

void logDumpJson(String &out) {
  uint16_t count = s_count;
  uint16_t idx   = oldestIndex();

  out.reserve(out.length() + (size_t)count * 80);
  out += '[';
  for (uint16_t i = 0; i < count; i++) {
    const LogEntry &e = s_ring[(idx + i) % LOG_RING_LINES];
    if (i) out += ',';
    out += "{\"t\":";
    out += e.ms;
    out += ",\"l\":";
    out += e.lvl;
    out += ",\"m\":\"";
    appendJsonEscaped(out, e.msg);
    out += "\"}";
  }
  out += ']';
}
