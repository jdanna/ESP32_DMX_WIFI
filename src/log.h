// Event log shown on the web UI's Log page: the last LOG_CAPACITY messages in
// PSRAM. Also captures ESP-IDF log output (USB host and audio driver errors),
// and echoes the firmware's own messages to Serial.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

enum LogLevel : uint8_t { LV_ERROR, LV_WARN, LV_INFO };

void logBegin();  // call first in setup()

void logError(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void logWarn(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void logInfo(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

uint32_t logSeq();       // sequence number of the newest entry, 0 = none yet
// Adds entries newer than `after`, oldest first, at most `limit` of them (the newest).
void logJson(JsonArray out, uint32_t after, int limit);
void logClear();
