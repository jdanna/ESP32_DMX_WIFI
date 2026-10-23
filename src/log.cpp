#include "log.h"

#include <esp_log.h>

namespace {

constexpr int LOG_CAPACITY = 300;
constexpr size_t TAG_LEN = 12, TEXT_LEN = 148;
const char *LEVEL_NAMES[] = {"E", "W", "I"};

struct Entry {
  uint32_t seq, ms;
  LogLevel level;
  char tag[TAG_LEN];
  char text[TEXT_LEN];
};

// Written from any task (IDF logs come from the USB tasks), so a spinlock
// guards the ring; formatting happens outside it.
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Entry *ring;
uint32_t seq, clearedSeq;
vprintf_like_t idfVprintf;

void store(LogLevel level, const char *tag, const char *text) {
  if (!ring) return;
  portENTER_CRITICAL(&mux);
  Entry &e = ring[seq % LOG_CAPACITY];
  e.seq = ++seq;
  e.ms = millis();
  e.level = level;
  strlcpy(e.tag, tag, sizeof(e.tag));
  strlcpy(e.text, text, sizeof(e.text));
  portEXIT_CRITICAL(&mux);
}

void writev(LogLevel level, const char *tag, const char *fmt, va_list args) {
  char text[TEXT_LEN];
  vsnprintf(text, sizeof(text), fmt, args);
  store(level, tag, text);
  Serial.printf("[%s] %s%s\n", tag, level == LV_ERROR ? "ERROR: " : level == LV_WARN ? "WARNING: " : "", text);
}

// IDF lines look like "E (1234) tag: message\n", optionally wrapped in colour codes.
int captureIdf(const char *fmt, va_list args) {
  char line[TEXT_LEN + TAG_LEN + 24];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(line, sizeof(line), fmt, copy);
  va_end(copy);

  char *p = line;
  if (*p == '\033') p = strchr(p, 'm') ? strchr(p, 'm') + 1 : p;
  LogLevel level = LV_INFO;
  if (*p == 'E') level = LV_ERROR;
  else if (*p == 'W') level = LV_WARN;
  char tag[TAG_LEN] = "idf";
  char *close = strchr(p, ')'), *colon = close ? strstr(close, ": ") : nullptr;
  if (*p && p[1] == ' ' && p[2] == '(' && close && colon) {
    const size_t n = min<size_t>(colon - close - 2, TAG_LEN - 1);
    memcpy(tag, close + 2, n);
    tag[n] = 0;
    p = colon + 2;
  }
  char *end = p + strlen(p);
  while (end > p && (end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
  if (char *esc = strchr(p, '\033')) *esc = 0;
  if (*p) store(level, tag, p);
  return idfVprintf ? idfVprintf(fmt, args) : 0;
}

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dipped)";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
  }
}

}  // namespace

void logBegin() {
  ring = (Entry *)ps_calloc(LOG_CAPACITY, sizeof(Entry));
  if (!ring) ring = (Entry *)calloc(LOG_CAPACITY / 4, sizeof(Entry));  // no PSRAM: shouldn't happen on this board
  idfVprintf = esp_log_set_vprintf(captureIdf);
  const esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_POWERON || r == ESP_RST_SW || r == ESP_RST_EXT) logInfo("sys", "Started after %s", resetReasonName(r));
  else logWarn("sys", "Started after %s", resetReasonName(r));
}

void logError(const char *tag, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  writev(LV_ERROR, tag, fmt, args);
  va_end(args);
}

void logWarn(const char *tag, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  writev(LV_WARN, tag, fmt, args);
  va_end(args);
}

void logInfo(const char *tag, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  writev(LV_INFO, tag, fmt, args);
  va_end(args);
}

uint32_t logSeq() { return seq; }

void logJson(JsonArray out, uint32_t after, int limit) {
  if (!ring) return;
  portENTER_CRITICAL(&mux);
  const uint32_t newest = seq;
  portEXIT_CRITICAL(&mux);
  uint32_t from = max(after, clearedSeq);
  if (newest > LOG_CAPACITY && from < newest - LOG_CAPACITY) from = newest - LOG_CAPACITY;
  if (newest - from > (uint32_t)limit) from = newest - limit;
  for (uint32_t s = from + 1; s <= newest; s++) {
    Entry e;
    portENTER_CRITICAL(&mux);
    e = ring[(s - 1) % LOG_CAPACITY];
    portEXIT_CRITICAL(&mux);
    if (e.seq != s) continue;  // overwritten while we were reading
    JsonObject o = out.add<JsonObject>();
    o["n"] = e.seq;
    o["t"] = e.ms;
    o["l"] = LEVEL_NAMES[e.level];
    o["g"] = e.tag;
    o["m"] = e.text;
  }
}

void logClear() {
  portENTER_CRITICAL(&mux);
  clearedSeq = seq;
  portEXIT_CRITICAL(&mux);
}
