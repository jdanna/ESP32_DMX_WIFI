// Persistent device configuration, stored as JSON in NVS.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>
#include "board.h"

#define FW_VERSION "1.1.0"
#ifdef ANIMATRONIC
#define FW_VARIANT "animatronic"
#else
#define FW_VARIANT "dmx-relay"
#endif

enum MergeMode : uint8_t { MERGE_HTP, MERGE_LTP, MERGE_DMX_FIRST, MERGE_ART_FIRST };
enum LossAction : uint8_t { LOSS_HOLD, LOSS_OFF };

struct IpSettings {
  bool dhcp = true;
  IPAddress ip, mask, gw, dns;
};

// Animatronic functions, each assignable to an input slot.
enum AnimFn : uint8_t { FN_LEFT, FN_RIGHT, FN_EYES, ANIM_FN_COUNT };

#define SFX_COUNT 4

// Sound-effect triggers and volume (ANIMATRONIC builds).
struct SfxSettings {
  uint8_t map[SFX_COUNT] = {0, 0, 0, 0};  // sound -> slot in range (1-8), 0 = unassigned
  uint8_t volSlot = 0;                    // slot whose level sets the volume, 0 = none
  uint8_t volume = 80;                    // percent, used while no volume channel is live
};

// Animatronic motor / limit-switch behaviour. Stored in every build so switching
// firmware variants keeps it; only ANIMATRONIC builds act on it.
struct AnimSettings {
  uint16_t reverseDelayMs = 500;  // both relays off this long before the motor changes direction
  uint16_t limitIgnoreMs = 300;   // limit-switch hits ignored this long after the motor starts
  uint16_t maxRunS = 30;          // stop if no limit is reached within this time (0 = never)
  bool limitNc = false;           // switch is normally closed (opens at the end of travel)
  bool swap = false;              // swap which relay drives left and right
  uint8_t map[ANIM_FN_COUNT] = {1, 2, 3};  // function -> slot in range (1-8), 0 = unassigned
  // Single-channel steering, used instead of the left/right slots when enabled:
  // below center - deadZone = left, above center + deadZone = right, in between = stop.
  bool singleDir = false;
  uint8_t dirSlot = 1;            // slot in range (1-8), 0 = unassigned
  uint8_t center = 128;
  uint8_t deadZone = 20;
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
  AnimSettings anim;
  SfxSettings sfx;
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
  bool usbAudio = true;           // USB host for a speaker; takes over the USB port (reboot to apply)
  char sfxNames[SFX_COUNT][33];   // names of the uploaded sound files
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

#ifdef ANIMATRONIC
// Each input slot may drive only one output: motor (left/right or direction),
// eyes, the four sounds, volume and relays 4-8. Checked when settings are
// saved, not when loaded, so older settings that share slots still boot.
bool configMappingExclusive(const Config &cfg, String &err);
#endif

const char *mergeName(MergeMode m);
const char *lossName(LossAction l);
