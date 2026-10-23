// Animatronic controller (ANIMATRONIC builds only). Relays 1 and 2 form an
// H-bridge for a reversing motor, relay 3 switches the eyes, and DI1 is the
// end-of-travel limit switch.
#pragma once

#include <Arduino.h>
#include "config.h"

enum MotorDir : uint8_t { MOTOR_STOP, MOTOR_LEFT, MOTOR_RIGHT };
enum StopReason : uint8_t { STOP_NONE, STOP_USER, STOP_LIMIT, STOP_TIMEOUT, STOP_INPUT };

constexpr uint8_t ANIM_RELAY_MASK = 0x07;  // relays 1-3 belong to the controller

struct AnimStatus {
  MotorDir dir;      // what the relays are doing now
  MotorDir target;   // where it's heading; differs from dir during the reversing pause
  MotorDir atLimit;  // direction blocked by the last limit stop, MOTOR_STOP if none
  StopReason reason; // why the motor last stopped
  bool limit;        // limit switch triggered (debounced, after the NO/NC setting)
  bool closed;       // raw contact state, for checking the wiring
  bool override;     // limit switch ignored
  bool eyes;
};

void animBegin();

// Runs every engine cycle. `inputMask` is what DMX/Art-Net ask the functions to do (bit = AnimFn);
// a change in it acts like pressing a button. `inputEnabled` is false in manual
// override, so the console can't move the motor then. Returns relay bits 0-2.
uint8_t animUpdate(uint32_t now, const AnimSettings &s, uint8_t inputMask, bool inputEnabled, AnimStatus &st);

// Web controls. animMotor refuses to drive further into a limit unless overridden.
bool animMotor(MotorDir dir, String &err);
void animEyes(bool on);
void animOverride(bool on);

const char *motorDirName(MotorDir d);
const char *stopReasonName(StopReason r);  // nullptr for STOP_NONE
