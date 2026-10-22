// Input -> relay engine. Runs in its own task: receives DMX frames and Art-Net
// packets, merges them, and drives the relays.
#pragma once

#include <IPAddress.h>
#include "config.h"

enum RelaySource : uint8_t { SRC_NONE, SRC_DMX, SRC_ART, SRC_MANUAL, SRC_HOLD };

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
  RelaySource source[RELAY_COUNT];
  uint8_t relayMask;
  bool manual;
  bool expanderOk;
};

void engineBegin(const IoSettings &io);
void engineApply(const IoSettings &io);     // takes effect on the next cycle
void engineSetManual(bool enabled);
void engineSetManualRelay(int index, bool on);
void engineSetManualAll(bool on);
EngineStatus engineStatus();
