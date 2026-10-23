#include "engine.h"

#include <NetworkUdp.h>
#include "dmx_rx.h"
#include "net.h"
#include "relays.h"

namespace {

constexpr uint16_t ART_PORT = 6454;
constexpr uint16_t OP_POLL = 0x2000;
constexpr uint16_t OP_POLL_REPLY = 0x2100;
constexpr uint16_t OP_DMX = 0x5000;
constexpr uint32_t REFRESH_MS = 1000;  // rewrite the expander periodically even without changes
#ifdef ANIMATRONIC
constexpr uint8_t ANIM_MASK = ANIM_RELAY_MASK;  // relays driven by the animatronic controller
#else
constexpr uint8_t ANIM_MASK = 0;
#endif

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
IoSettings pending;
bool pendingNew = false;
volatile bool manual = false;
volatile uint8_t manualMask = 0;
EngineStatus shared = {};

// Owned by the engine task.
IoSettings io;
NetworkUDP udp;
uint8_t dmxFrame[DMX_FRAME_SIZE];
uint16_t dmxSlots;
uint32_t dmxLastFrame, dmxLastMs, dmxCount, dmxWindowCount;
uint8_t artData[512];
uint16_t artSlots;
uint32_t artLastMs, artCount, artWindowCount, artPollCount;
IPAddress artFrom;
bool everDmx, everArt;
uint8_t relayMask, outMask, writtenMask;  // relayMask: what the inputs ask for; outMask: what's driven
uint8_t fnMask;                           // animatronic functions the inputs ask for, bit = AnimFn
uint8_t sfxMask;                          // sound triggers the inputs ask for
int16_t inputVolume = -1;                 // last volume-channel level, -1 = none
bool expanderOk;
uint32_t lastWriteMs, windowStartMs;
uint16_t dmxFps, artFps;
RelaySource sources[RELAY_COUNT];

uint8_t packet[600];

void sendPollReply(const IPAddress &to) {
  IPAddress ip;
  uint8_t mac[6] = {};
  bool dhcp = false;
  if (!netLocalFor(to, ip, mac, dhcp)) return;

  uint8_t r[239] = {};
  memcpy(r, "Art-Net", 8);
  r[8] = OP_POLL_REPLY & 0xff;
  r[9] = OP_POLL_REPLY >> 8;
  for (int i = 0; i < 4; i++) r[10 + i] = ip[i];
  r[14] = ART_PORT & 0xff;
  r[15] = ART_PORT >> 8;
  r[16] = 1;  // firmware version
  r[17] = 0;
  r[18] = (io.artUniverse >> 8) & 0x7f;  // Net
  r[19] = (io.artUniverse >> 4) & 0x0f;  // Sub-Net
  r[20] = 0xff;                          // OEM: unregistered
  r[21] = 0xff;
  r[23] = 0xd0;  // indicators normal, addresses set from the web UI
  strlcpy((char *)r + 26, g_cfg.hostname, 18);
  snprintf((char *)r + 44, 64, "DMX Relay Node (%s) ESP32-S3-POE-ETH-8DI-8RO", g_cfg.hostname);
  snprintf((char *)r + 108, 64, "#0001 [%04lu] OK, %s", (unsigned long)(++artPollCount % 10000),
           io.artEnabled ? "listening" : "Art-Net input disabled");
  r[173] = 1;                                      // one port
  r[174] = 0x80;                                   // it outputs DMX512 from Art-Net
  bool live = everArt && millis() - artLastMs < io.lossTimeoutMs;
  r[182] = live ? 0x80 : 0x00;                     // GoodOutput: data being transmitted
  r[190] = io.artUniverse & 0x0f;                  // SwOut
  r[200] = 0x00;                                   // StNode
  memcpy(r + 201, mac, 6);
  for (int i = 0; i < 4; i++) r[207 + i] = ip[i];  // BindIp
  r[211] = 1;                                      // BindIndex
  r[212] = 0x08 | 0x04 | (dhcp ? 0x02 : 0);        // 15-bit addresses, DHCP capable, DHCP in use

  udp.beginPacket(to, ART_PORT);
  udp.write(r, sizeof(r));
  udp.endPacket();
}

void handleArtNet(int len, const IPAddress &from) {
  if (len < 12 || memcmp(packet, "Art-Net", 8) != 0) return;
  const uint16_t op = packet[8] | (packet[9] << 8);

  if (op == OP_POLL) {
    sendPollReply(from);
    return;
  }
  if (op != OP_DMX || !io.artEnabled || len < 18) return;

  const uint16_t portAddress = packet[14] | ((packet[15] & 0x7f) << 8);
  if (portAddress != io.artUniverse) return;
  uint16_t slots = (packet[16] << 8) | packet[17];
  if (slots > 512) slots = 512;
  if (slots > len - 18) slots = len - 18;
  memcpy(artData, packet + 18, slots);
  artSlots = slots;
  artLastMs = millis();
  artFrom = from;
  everArt = true;
  artCount++;
  artWindowCount++;
}

void pollInputs(bool udpOpen) {
  for (int i = 0; udpOpen && i < 16; i++) {  // bounded so a flood of other universes can't starve us
    int size = udp.parsePacket();
    if (size <= 0) break;
    int len = udp.read(packet, sizeof(packet));
    if (len > 0) handleArtNet(len, udp.remoteIP());
  }

  uint32_t fc = dmxFrameCount();
  if (fc != dmxLastFrame) {
    dmxLastFrame = fc;
    if (io.dmxEnabled) {
      dmxSlots = dmxCopyFrame(dmxFrame);
      dmxLastMs = millis();
      everDmx = true;
      dmxCount++;
      dmxWindowCount++;
    }
  }
}

// Value of `slot` (1-8) of the configured range, or -1 if the input isn't live.
int dmxValue(int slot, bool live) {
  if (!live || !slot) return -1;
  int ch = io.dmxStart + slot - 1;
  return ch <= dmxSlots ? dmxFrame[ch] : 0;  // dmxFrame[0] is the start code
}

int artValue(int slot, bool live) {
  if (!live || !slot) return -1;
  int ch = io.artStart + slot - 1;
  return ch <= artSlots ? artData[ch - 1] : 0;
}

// Which input supplies a slot's value while at least one input is live.
bool useDmx(bool dmxLive, bool artLive, int dv, int av) {
  switch (io.merge) {
    case MERGE_LTP: return dmxLive && (!artLive || (int32_t)(dmxLastMs - artLastMs) >= 0);
    case MERGE_DMX_FIRST: return dmxLive;
    case MERGE_ART_FIRST: return !artLive;
    default: return dv >= av;  // HTP: highest value wins
  }
}

// Next state of an output driven by input `slot` (0 = unassigned), given whether it's on now.
bool decide(int slot, bool on, bool dmxLive, bool artLive, int dv, int av, RelaySource &src) {
  if (!dmxLive && !artLive) {
    if (io.loss == LOSS_OFF || !on) {
      src = SRC_NONE;
      return false;
    }
    src = SRC_HOLD;
    return true;
  }
  if (!slot) {
    src = SRC_NONE;
    return false;
  }

  const bool dmx = useDmx(dmxLive, artLive, dv, av);
  src = dmx ? SRC_DMX : SRC_ART;
  return (dmx ? dv : av) >= io.threshold;
}

void evaluate(uint32_t now, EngineStatus &st) {
  const bool dmxLive = io.dmxEnabled && everDmx && now - dmxLastMs < io.lossTimeoutMs;
  const bool artLive = io.artEnabled && everArt && now - artLastMs < io.lossTimeoutMs;
  const bool man = manual;
  const uint8_t manMask = manualMask;

  for (int i = 0; i < RELAY_COUNT; i++) {
    const int slot = io.map[i];
    const int dv = dmxValue(slot, dmxLive);
    const int av = artValue(slot, artLive);
    st.dmxValue[i] = dv;
    st.artValue[i] = av;
    const uint8_t bit = 1 << i;

    if (man) {
      relayMask = (relayMask & ~bit) | (manMask & bit);
      sources[i] = SRC_MANUAL;
      continue;
    }
    if (decide(slot, relayMask & bit, dmxLive, artLive, dv, av, sources[i])) relayMask |= bit;
    else relayMask &= ~bit;
  }

  for (int s = 1; s <= RELAY_COUNT; s++) {
    st.inDmx[s - 1] = dmxValue(s, dmxLive);
    st.inArt[s - 1] = artValue(s, artLive);
  }

#ifdef ANIMATRONIC
  const AnimSettings &an = io.anim;
  for (int f = 0; f < ANIM_FN_COUNT; f++) {
    if (an.singleDir && (f == FN_LEFT || f == FN_RIGHT)) continue;
    const int slot = an.map[f];
    const uint8_t bit = 1 << f;
    RelaySource src;
    if (decide(slot, fnMask & bit, dmxLive, artLive, dmxValue(slot, dmxLive), artValue(slot, artLive), src))
      fnMask |= bit;
    else fnMask &= ~bit;
  }

  if (an.singleDir) {
    constexpr uint8_t MOTOR_BITS = (1 << FN_LEFT) | (1 << FN_RIGHT);
    const int slot = an.dirSlot;
    if (dmxLive || artLive) {
      fnMask &= ~MOTOR_BITS;
      if (slot) {
        const int dv = dmxValue(slot, dmxLive), av = artValue(slot, artLive);
        const int v = useDmx(dmxLive, artLive, dv, av) ? dv : av;
        if (v < an.center - an.deadZone) fnMask |= 1 << FN_LEFT;
        else if (v > an.center + an.deadZone) fnMask |= 1 << FN_RIGHT;
      }
    } else if (io.loss == LOSS_OFF) {
      fnMask &= ~MOTOR_BITS;
    }
  }

  for (int k = 0; k < SFX_COUNT; k++) {
    const int slot = io.sfx.map[k];
    const uint8_t bit = 1 << k;
    RelaySource src;
    if (decide(slot, sfxMask & bit, dmxLive, artLive, dmxValue(slot, dmxLive), artValue(slot, artLive), src))
      sfxMask |= bit;
    else sfxMask &= ~bit;
  }
  // The volume channel overrides the web volume while live, and holds or drops on signal loss like a relay.
  const int vslot = io.sfx.volSlot;
  if (vslot && (dmxLive || artLive)) {
    const int dv = dmxValue(vslot, dmxLive), av = artValue(vslot, artLive);
    inputVolume = useDmx(dmxLive, artLive, dv, av) ? dv : av;
  } else if (!vslot || io.loss == LOSS_OFF) {
    inputVolume = -1;
  }
  const uint8_t volume = inputVolume >= 0 ? (inputVolume * 100 + 127) / 255 : io.sfx.volume;
  sfxInput(sfxMask, !man, volume, inputVolume >= 0);
#endif

  st.dmx = {dmxLive, dmxFps, dmxCount, dmxErrorCount(), dmxSlots};
  st.art = {artLive, artFps, artCount, 0, artSlots};
  st.artFrom = artFrom;
  memcpy(st.source, sources, sizeof(sources));
  st.manual = man;
}

void engineTask(void *) {
  bool udpOpen = false;
  windowStartMs = millis();

  for (;;) {
    if (!udpOpen) udpOpen = udp.begin(ART_PORT);

    portENTER_CRITICAL(&mux);
    if (pendingNew) {
      io = pending;
      pendingNew = false;
    }
    portEXIT_CRITICAL(&mux);

    pollInputs(udpOpen);

    const uint32_t now = millis();
    if (now - windowStartMs >= 1000) {
      dmxFps = dmxWindowCount * 1000 / (now - windowStartMs);
      artFps = artWindowCount * 1000 / (now - windowStartMs);
      dmxWindowCount = artWindowCount = 0;
      windowStartMs = now;
    }

    EngineStatus st = {};
    evaluate(now, st);
    outMask = relayMask;
#ifdef ANIMATRONIC
    outMask = (relayMask & ~ANIM_MASK) | animUpdate(now, io.anim, fnMask, !st.manual, st.anim);
    if ((outMask & 0x03) == 0x03) outMask &= ~0x03;  // last-ditch guard: never both motor relays at once
    // Relay tiles 1-3 show the levels of the function driving them.
    const uint8_t fnOf[3] = {uint8_t(io.anim.swap ? FN_RIGHT : FN_LEFT), uint8_t(io.anim.swap ? FN_LEFT : FN_RIGHT), FN_EYES};
    for (int i = 0; i < 3; i++) {
      const int slot = io.anim.singleDir && fnOf[i] != FN_EYES ? io.anim.dirSlot : io.anim.map[fnOf[i]];
      st.dmxValue[i] = slot ? st.inDmx[slot - 1] : -1;
      st.artValue[i] = slot ? st.inArt[slot - 1] : -1;
      st.source[i] = SRC_ANIM;
    }
#endif
    st.relayMask = outMask;

    if (outMask != writtenMask || now - lastWriteMs >= REFRESH_MS) {
      expanderOk = relaysWrite(outMask);
      if (expanderOk) writtenMask = outMask;
      lastWriteMs = now;
    }
    st.expanderOk = expanderOk;

    portENTER_CRITICAL(&mux);
    shared = st;
    portEXIT_CRITICAL(&mux);

    vTaskDelay(1);
  }
}

}  // namespace

void engineBegin(const IoSettings &settings) {
  io = settings;
  expanderOk = relaysBegin();
  if (!expanderOk) log_e("TCA9554 relay expander not responding");
#ifdef ANIMATRONIC
  animBegin();
#endif
  xTaskCreatePinnedToCore(engineTask, "engine", 6144, nullptr, 5, nullptr, 1);
}

void engineApply(const IoSettings &settings) {
  portENTER_CRITICAL(&mux);
  pending = settings;
  pendingNew = true;
  portEXIT_CRITICAL(&mux);
}

void engineSetManual(bool enabled) {
  if (enabled && !manual) manualMask = engineStatus().relayMask;  // start from what's on now
  manual = enabled;
}

void engineSetManualRelay(int index, bool on) {
  if (index < 0 || index >= RELAY_COUNT) return;
  uint8_t bit = 1 << index;
  manualMask = on ? (manualMask | bit) : (manualMask & ~bit);
}

void engineSetManualAll(bool on) { manualMask = on ? 0xff : 0x00; }

EngineStatus engineStatus() {
  portENTER_CRITICAL(&mux);
  EngineStatus st = shared;
  portEXIT_CRITICAL(&mux);
  return st;
}
