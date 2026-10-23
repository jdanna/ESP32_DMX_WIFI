// DMX / Art-Net relay node for the Waveshare ESP32-S3-POE-ETH-8DI-8RO.
//
// DMX on the RS485 port and Art-Net on Ethernet or Wi-Fi each listen to an
// 8-channel range; those channels switch the eight relays. Everything is
// configured from the built-in web page.

#include <Arduino.h>
#include "board.h"
#include "config.h"
#include "dmx_rx.h"
#include "engine.h"
#include "log.h"
#include "net.h"
#include "web.h"
#ifdef ANIMATRONIC
#include "sfx.h"
#endif

static constexpr uint32_t FACTORY_RESET_HOLD_MS = 5000;

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xffffffff;
  uint32_t rgb = (r << 16) | (g << 8) | b;
  if (rgb == last) return;
  last = rgb;
  // Waveshare's own demo swaps red/green for this LED; change the order here if colours look wrong.
  rgbLedWriteOrdered(PIN_STATUS_LED, LED_COLOR_ORDER_RGB, r, g, b);
}

static void updateLed() {
  const EngineStatus st = engineStatus();
  const bool blink = (millis() / 500) % 2;
  if (!st.expanderOk) setLed(blink ? 40 : 0, 0, 0);  // relay expander fault: red flash
  else if (st.manual) setLed(40, 18, 0);             // manual override: orange
  else if (st.dmx.live || st.art.live) setLed(0, 30, 22);  // receiving: teal
  else if (netApActive()) setLed(0, 0, blink ? 40 : 4);    // setup AP: blue flash
  else if (netHasIp()) setLed(0, 6, 12);                   // online, idle: dim blue
  else setLed(30, 16, 0);                                  // no network yet: amber
}

static void checkResetButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(PIN_BOOT_BTN) == HIGH) {
    pressedAt = 0;
    return;
  }
  if (!pressedAt) pressedAt = millis();
  if (millis() - pressedAt < FACTORY_RESET_HOLD_MS) return;

  logWarn("sys", "BOOT held - factory reset");
  configErase();
  for (int i = 0; i < 6; i++) {
    setLed(i % 2 ? 0 : 60, 0, 0);
    delay(150);
  }
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  logBegin();
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  setLed(30, 16, 0);

  configLoad(g_cfg);
  logInfo("sys", "DMX Relay Node %s (%s) - %s", FW_VERSION, FW_VARIANT, g_cfg.hostname);

  dmxBegin(PIN_RS485_TX, PIN_RS485_RX);
  netBegin(g_cfg);
#ifdef ANIMATRONIC
  sfxBegin(g_cfg.usbAudio);
#endif
  engineBegin(g_cfg.io);
  webBegin();
  logInfo("sys", "Web UI at http://%s.local/", g_cfg.hostname);
}

void loop() {
  webLoop();
  netLoop();
  updateLed();
  checkResetButton();
#ifdef ANIMATRONIC
  animLoop();
#endif
  delay(2);
}
