// Input -> relay engine. Runs in its own task: receives DMX frames and Art-Net
// packets, merges them, and drives the relays.
#pragma once

#include <IPAddress.h>
#include "config.h"
#ifdef ANIMATRONIC
#include "anim.h"
#include "sfx.h"
#endif

enum RelaySource : uint8_t { SRC_NONE, SRC_DMX, SRC_ART, SRC_MANUAL, SRC_HOLD, SRC_ANIM, SRC_KEY };

struct InputStatus {
  bool live;
  uint16_t fps;
  uint32_t count;     // frames / packets accepted
  uint32_t errors;
  uint16_t slots;     // slots in the last frame
};

struct EngineStatus {
  InputStatus dmx, art;
  IPAddress artFrom;
  int16_t dmxValue[RELAY_COUNT];  // -1 when the input isn't live or the relay is unassigned
  int16_t artValue[RELAY_COUNT];
  int16_t inDmx[RELAY_COUNT];     // raw values of the 8 input slots, -1 when the input isn't live
  int16_t inArt[RELAY_COUNT];
  RelaySource source[RELAY_COUNT];
  uint8_t relayMask;
  bool manual;
  bool expanderOk;
#ifdef ANIMATRONIC
  AnimStatus anim;
#endif
};

void engineBegin(const IoSettings &io);
void engineApply(const IoSettings &io);     // takes effect on the next cycle
void engineSetManual(bool enabled);
void engineSetManualRelay(int index, bool on);
void engineSetManualAll(bool on);
// Web keyboard: bit n = number key n+1 held, which acts like input channel n+1 at full.
// The page repeats it while keys are held; it lapses if not repeated.
void engineSetKeys(uint8_t mask);
EngineStatus engineStatus();
