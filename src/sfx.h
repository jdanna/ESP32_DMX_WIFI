// Sound effects (ANIMATRONIC builds): four WAV slots stored in LittleFS and
// held in PSRAM, mixed and played through a USB Audio Class speaker connected
// to the board's USB-C port.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

void sfxBegin(bool usbAudio);

// Called every engine cycle: `mask` has a bit per sound whose input is on (a
// rising edge plays it), `enabled` is false in manual override, `volume` is a
// percent and `volumeFromInput` says whether a DMX/Art-Net channel set it.
void sfxInput(uint8_t mask, bool enabled, uint8_t volume, bool volumeFromInput);

void sfxPlay(int slot);
void sfxStop(int slot);  // -1 stops all
void sfxTone();          // 3 s 440 Hz test tone straight to the speaker, ignoring the volume

// Upload support: the file is written to sfxUploadPath() first, then
// sfxInstall() checks it, moves it into the slot and loads it.
const char *sfxUploadPath();
bool sfxInstall(int slot, String &err);
void sfxDelete(int slot);

void sfxStatusJson(JsonObject out);
uint8_t sfxPlayingMask();
uint8_t sfxVolume();
bool sfxVolumeFromInput();
