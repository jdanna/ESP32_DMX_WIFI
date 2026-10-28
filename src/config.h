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

// Input channels: a range of this many consecutive DMX slots / Art-Net channels.
#define INPUT_COUNT 16

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
  uint8_t map[SFX_COUNT] = {9, 10, 11, 12};  // sound -> slot in range (1-16), 0 = unassigned
  uint8_t volSlot = 8;                       // slot whose level sets the volume, 0 = none
  uint8_t volume = 80;                    // percent, used while no volume channel is live
};

#define RANDOM_INTERVAL_MAX_MS 20000  // random interval slider and channel at full

// Animatronic motor / center-switch behaviour. Stored in every build so switching
// firmware variants keeps it; only ANIMATRONIC builds act on it.
struct AnimSettings {
  uint16_t reverseDelayMs = 250;  // both relays off this long before the motor changes direction
  uint16_t maxSwingMs = 2000;     // furthest the head runs either side of center, also the homing search limit
  bool homeOnBoot = false;        // find center by itself after power-up
  bool random = false;            // random head moves and eye changes
  uint16_t randomIntervalMs = 6000;  // typical pause between random behaviours; each one is 0.5-1.5x this
  // Channels that override the two random settings while an input is live, 0 = unassigned:
  // on/off switches at the threshold, the interval channel's 0-255 maps to 0-20 s.
  uint8_t randomSlot = 6;
  uint8_t randomTimeSlot = 7;
  // Eyes on move: eyes on while the head moves and for 2 s after. Sound on move: a random sound
  // when a move starts, then quiet until 1 s after it ends. Each has a channel that switches it
  // at the threshold, overriding the setting while an input is live.
  bool eyesOnMove = false;
  bool soundOnMove = false;
  uint8_t eyesMoveSlot = 4;
  uint8_t soundMoveSlot = 5;
  uint8_t centerSlot = 2;         // rising past the threshold drives the head back to center
  bool limitNc = false;           // center switch is normally closed (opens at center)
  bool swap = false;              // swap which relay drives left and right
  uint8_t map[ANIM_FN_COUNT] = {0, 0, 3};  // function -> slot in range (1-16), 0 = unassigned
  // Single-channel steering, used instead of the left/right slots when enabled:
  // below center - deadZone = left, above center + deadZone = right, in between = stop.
  bool singleDir = true;
  uint8_t dirSlot = 1;            // slot in range (1-16), 0 = unassigned
  uint8_t center = 128;
  uint8_t deadZone = 20;
};

// Everything the relay engine needs; copied into the control task on change.
struct IoSettings {
  bool dmxEnabled = true;
  uint16_t dmxStart = 1;          // first DMX slot of the 16-channel range (1-497)
  bool artEnabled = true;
  uint16_t artUniverse = 0;       // 15-bit Art-Net port-address (Net:SubNet:Universe)
  uint16_t artStart = 1;          // first channel within that universe (1-497)
  uint8_t map[RELAY_COUNT];       // relay -> slot in range (1-16), 0 = unassigned
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

constexpr uint16_t RANGE_MAX_START = 512 - INPUT_COUNT + 1;

extern Config g_cfg;
extern uint32_t g_cfgRev;  // bumped on every save, so open pages know to reload the settings

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
// eyes, center, eyes/sound on move, random mode on/off and interval, the four sounds,
// volume and relays 4-8. Checked when settings are
// saved, not when loaded, so older settings that share slots still boot.
bool configMappingExclusive(const Config &cfg, String &err);
#endif

const char *mergeName(MergeMode m);
const char *lossName(LossAction l);
