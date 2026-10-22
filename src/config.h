// Persistent device configuration, stored as JSON in NVS.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>
#include "board.h"

#define FW_VERSION "1.0.0"

enum MergeMode : uint8_t { MERGE_HTP, MERGE_LTP, MERGE_DMX_FIRST, MERGE_ART_FIRST };
enum LossAction : uint8_t { LOSS_HOLD, LOSS_OFF };

struct IpSettings {
  bool dhcp = true;
  IPAddress ip, mask, gw, dns;
};

// Everything the relay engine needs; copied into the control task on change.
struct IoSettings {
  bool dmxEnabled = true;
  uint16_t dmxStart = 1;          // first DMX slot of the 8-channel range (1-505)
  bool artEnabled = true;
  uint16_t artUniverse = 0;       // 15-bit Art-Net port-address (Net:SubNet:Universe)
  uint16_t artStart = 1;          // first channel within that universe (1-505)
  uint8_t map[RELAY_COUNT];       // relay -> slot in range (1-8), 0 = unassigned
  uint8_t threshold = 128;        // channel value at or above which a relay closes
  MergeMode merge = MERGE_HTP;
  LossAction loss = LOSS_HOLD;
  uint16_t lossTimeoutMs = 5000;  // input is considered lost after this long without data
};

struct Config {
  char hostname[32];
  IpSettings eth;
  bool wifiEnabled = false;
  char wifiSsid[33];
  char wifiPass[65];
  IpSettings wifi;
  IoSettings io;
  char relayNames[RELAY_COUNT][25];
};

constexpr uint16_t RANGE_MAX_START = 512 - RELAY_COUNT + 1;

extern Config g_cfg;

void configDefaults(Config &cfg);
void configLoad(Config &cfg);
void configSave(const Config &cfg);
void configErase();

// JSON (de)serialisation shared by NVS storage and the web API.
// includeSecrets: write the Wi-Fi password (NVS only, never sent to the browser).
void configToJson(const Config &cfg, JsonObject out, bool includeSecrets);
// Applies whatever keys are present in `in` on top of `cfg`. Returns false and
// fills `err` on invalid input; sets `networkChanged` if a reboot is needed.
bool configFromJson(Config &cfg, JsonObjectConst in, String &err, bool &networkChanged);

const char *mergeName(MergeMode m);
const char *lossName(LossAction l);
