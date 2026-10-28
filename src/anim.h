// Animatronic controller (ANIMATRONIC builds only). Relays 1 and 2 form an
// H-bridge for a reversing motor that swings the head, relay 3 switches the
// eyes, and DI1 is a switch that closes when the head is centered.
//
// Homing sweeps left and right from wherever the head is, each swing longer
// than the last (up to the max swing time), until the center switch closes.
// After that the head's position is tracked by run time, re-zeroed every time
// it passes center, and moves stop at the max swing time either side of center.
// The last position is saved, so after a restart homing runs straight toward
// center first and only falls back to the widening search if it isn't there.
//
// Random mode turns the head to random points within the travel, at random
// intervals around the configured time. It doesn't touch the eyes.
//
// Eyes on move (a setting) also lights the eyes while the head moves and for a few
// seconds after, on top of whatever the eyes control says.
#pragma once

#include <Arduino.h>
#include "config.h"

enum MotorDir : uint8_t { MOTOR_STOP, MOTOR_LEFT, MOTOR_RIGHT };
enum StopReason : uint8_t { STOP_NONE, STOP_USER, STOP_TRAVEL, STOP_TIMEOUT, STOP_INPUT, STOP_HOMED, STOP_HOME_FAILED };
enum HomeState : uint8_t { HOME_NONE, HOME_SEARCHING, HOME_OK, HOME_FAILED };

constexpr uint8_t ANIM_RELAY_MASK = 0x07;  // relays 1-3 belong to the controller

struct AnimStatus {
  MotorDir dir;      // what the relays are doing now
  MotorDir target;   // where it's heading; differs from dir during the reversing pause
  MotorDir atLimit;  // direction blocked at the end of travel, MOTOR_STOP if none
  StopReason reason; // why the motor last stopped
  bool center;       // center switch active (debounced, after the NO/NC setting)
  bool closed;       // raw contact state, for checking the wiring
  bool override;     // travel limits ignored
  bool eyes;
  HomeState home;
  int32_t posMs;     // run time from center, left negative; meaningful once homed
  bool centering;    // driving back to center
  bool random;       // random mode running (on, and not paused by manual override)
  // Filled in by the engine, which applies the random mode channels:
  bool randomOn;          // random mode wanted, by the setting or its channel (may be paused)
  bool randomFromInput;   // on/off comes from its channel
  uint16_t randomMs;      // interval in use
  bool randomMsFromInput; // interval comes from its channel
  bool eyesOnMove, eyesOnMoveFromInput;    // in effect, and whether its channel set it
  bool soundOnMove, soundOnMoveFromInput;
};

void animBegin();
void animLoop();  // from the main loop: saves the last known position to flash

// Runs every engine cycle. `inputMask` is what DMX/Art-Net ask the functions to do (bit = AnimFn);
// a change in it acts like pressing a button. `inputEnabled` is false in manual
// override, so neither the console nor random mode can move the motor then. Returns relay bits 0-2.
uint8_t animUpdate(uint32_t now, const AnimSettings &s, uint8_t inputMask, bool inputEnabled, AnimStatus &st);

// Web controls. animMotor refuses to drive further past the travel limit unless overridden,
// and cancels homing.
bool animMotor(MotorDir dir, String &err);
void animHome();
void animCenter();  // drive back to center; homes first if the position isn't known
void animEyes(bool on);
void animOverride(bool on);

const char *motorDirName(MotorDir d);
const char *stopReasonName(StopReason r);  // nullptr for STOP_NONE
const char *homeStateName(HomeState h);
