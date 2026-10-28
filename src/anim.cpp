#include "anim.h"

#include <Preferences.h>
#include <esp_random.h>
#include "board.h"
#include "log.h"

namespace {

constexpr uint32_t DEBOUNCE_MS = 5;          // short: the switch is only closed briefly as the head swings past
constexpr int32_t HOME_STEP_MS = 250;        // each homing swing reaches this much further than the last
constexpr uint32_t BOOT_HOME_DELAY_MS = 2000;
constexpr uint32_t SAVE_AFTER_MS = 1000;     // save the position once the head has been still this long
constexpr int32_t SAVE_MIN_CHANGE_MS = 20;   // ...and it moved at least this much since the last save
const char *NVS_NS = "anim", *NVS_POS = "pos";
constexpr int32_t RANDOM_REACH_PCT = 80;     // random targets stay within this much of the max swing
constexpr uint32_t RANDOM_START_MS = 500;    // first random change this soon after turning it on
constexpr uint32_t EYES_HOLD_MS = 5000;      // eyes on move: stay lit this long after the head stops

const char *DIR_NAMES[] = {"stop", "left", "right"};
const char *REASON_NAMES[] = {nullptr, "user", "travel", "timeout", "input", "homed", "homeFailed"};
const char *HOME_NAMES[] = {"none", "searching", "ok", "failed"};

// Shared between the engine task and the web handlers.
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
MotorDir target = MOTOR_STOP;
StopReason reason = STOP_NONE;
HomeState home = HOME_NONE;
bool override = false, eyes = false, homeRequested = false, centerRequested = false, posKnown = false;
int32_t posMs, limitMs = 2500;
// Last known position, kept in NVS across restarts so homing can head for center
// first. The engine task decides what to save; animLoop() does the slow flash write.
enum SaveOp : uint8_t { SAVE_NONE, SAVE_WRITE, SAVE_CLEAR };
bool hasSaved;
int32_t savedPos;
SaveOp saveOp = SAVE_NONE;

// Owned by the engine task.
MotorDir dir = MOTOR_STOP, lastMoveDir = MOTOR_STOP;
uint32_t stopMs, runStartMs, rawSince, lastMs, idleSince, moveEyesUntil;
bool rawPrev, closed, centerPrev, bootHomeChecked;
uint8_t inputPrev;
// Driving back to center once homed; stops at the switch, or gives up this far past it.
bool centering;
int32_t centerOverrunMs;
// Random mode: the next change is due at randomDueMs; gotoActive while the head
// runs to gotoMs. Engine task only, except gotoActive which request() clears.
bool randomPrev, randomArm, gotoActive;
uint32_t randomDueMs;
int32_t gotoMs;
// Homing search, relative to where it started. directMs > 0 while making the first,
// straight run toward where the saved position says center is.
int32_t relMs, ampMs, directMs;
bool leftDone, rightDone, returning;
// Things to log once the spinlock is released (logging can block).
enum : uint8_t {
  EV_CENTERED = 1, EV_HOMING = 2, EV_HOME_FAILED = 4, EV_HOMING_SAVED = 8, EV_SAVED_MISSED = 16, EV_CENTER_MISSED = 32,
  EV_RANDOM_ON = 64, EV_RANDOM_OFF = 128
};
uint8_t events;

// Caller holds mux.
MotorDir limitDir() {
  if (!posKnown || override) return MOTOR_STOP;
  return posMs <= -limitMs ? MOTOR_LEFT : posMs >= limitMs ? MOTOR_RIGHT : MOTOR_STOP;
}

// Caller holds mux.
bool request(MotorDir want, StopReason why) {
  if (want != MOTOR_STOP && want == limitDir()) return false;
  if (home == HOME_SEARCHING) home = posKnown ? HOME_OK : HOME_NONE;  // a move or stop cancels homing
  centering = false;
  gotoActive = false;
  target = want;
  if (want == MOTOR_STOP) reason = why;
  return true;
}

// Caller holds mux.
void foundCenter() {
  posMs = 0;
  if (!posKnown) events |= EV_CENTERED;
  posKnown = true;
}

// Caller holds mux.
void startHoming(bool center) {
  if (center) {
    foundCenter();
    home = HOME_OK;
    target = MOTOR_STOP;
    reason = STOP_HOMED;
    return;
  }
  home = HOME_SEARCHING;
  relMs = 0;
  ampMs = min(HOME_STEP_MS, limitMs);
  leftDone = rightDone = returning = false;
  if (hasSaved && abs(savedPos) >= HOME_STEP_MS) {
    // Run straight toward center, allowing a margin for drift and coasting, rather
    // than swinging both ways and possibly further out past the end of travel.
    directMs = min(abs(savedPos) + max(2 * HOME_STEP_MS, abs(savedPos) / 4), 2 * limitMs);
    target = savedPos < 0 ? MOTOR_RIGHT : MOTOR_LEFT;
    events |= EV_HOMING_SAVED;
  } else {
    directMs = 0;
    target = MOTOR_LEFT;
    events |= EV_HOMING;
  }
}

uint32_t rnd(uint32_t n) { return esp_random() % n; }

uint32_t randomDelay(const AnimSettings &s) { return s.randomIntervalMs / 2 + rnd(s.randomIntervalMs + 1); }

// Caller holds mux.
void startCentering(bool center) {
  if (center) {
    target = MOTOR_STOP;
    reason = STOP_HOMED;
    return;
  }
  centering = true;
  centerOverrunMs = max(2 * HOME_STEP_MS, abs(posMs) / 4);
  target = posMs < 0 ? MOTOR_RIGHT : MOTOR_LEFT;
}

// Caller holds mux. One random turn of the head; every few turns, look straight ahead.
void randomStep(bool center) {
  if (!posKnown) return;
  const int32_t reach = limitMs * RANDOM_REACH_PCT / 100;
  if (rnd(5) == 0 && abs(posMs) > HOME_STEP_MS) {
    startCentering(center);
    return;
  }
  // Somewhere else in the range, far enough from here that it's a real turn.
  int32_t to = posMs;
  for (int i = 0; i < 8 && abs(to - posMs) < limitMs / 4; i++) to = (int32_t)rnd(2 * reach + 1) - reach;
  if (abs(to - posMs) < limitMs / 4) return;
  gotoActive = true;
  gotoMs = to;
  target = to < posMs ? MOTOR_LEFT : MOTOR_RIGHT;
}

// Caller holds mux. Called when the current swing reaches its end.
void nextSwing() {
  if (ampMs >= limitMs) (dir == MOTOR_LEFT ? leftDone : rightDone) = true;
  if (leftDone && rightDone) {
    returning = true;  // searched the full swing both ways: go back to where it started
    target = relMs < 0 ? MOTOR_RIGHT : MOTOR_LEFT;
    return;
  }
  ampMs = min(ampMs + HOME_STEP_MS, limitMs);
  target = dir == MOTOR_LEFT ? MOTOR_RIGHT : MOTOR_LEFT;
}

}  // namespace

void animBegin() {
  pinMode(PIN_DI1, INPUT_PULLUP);
  rawPrev = closed = digitalRead(PIN_DI1) == LOW;
  lastMs = millis();
  Preferences prefs;
  if (prefs.begin(NVS_NS, true)) {
    hasSaved = prefs.isKey(NVS_POS);
    savedPos = prefs.getInt(NVS_POS, 0);
    prefs.end();
  }
}

void animLoop() {
  portENTER_CRITICAL(&mux);
  const SaveOp op = saveOp;
  const int32_t pos = savedPos;
  saveOp = SAVE_NONE;
  portEXIT_CRITICAL(&mux);
  if (op == SAVE_NONE) return;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, false)) return;
  if (op == SAVE_WRITE) prefs.putInt(NVS_POS, pos);
  else prefs.remove(NVS_POS);
  prefs.end();
}

uint8_t animUpdate(uint32_t now, const AnimSettings &s, uint8_t inputMask, bool inputEnabled, AnimStatus &st) {
  const bool raw = digitalRead(PIN_DI1) == LOW;
  if (raw != rawPrev) {
    rawPrev = raw;
    rawSince = now;
  }
  if (now - rawSince >= DEBOUNCE_MS) closed = raw;
  const bool center = closed != s.limitNc;
  const bool centerHit = center && !centerPrev;
  centerPrev = center;

  const int32_t dt = now - lastMs;
  lastMs = now;
  const uint8_t in = inputMask & ANIM_RELAY_MASK;

  portENTER_CRITICAL(&mux);
  limitMs = s.maxSwingMs;
  // Position by run time; coasting after the relays drop isn't counted, so it's
  // corrected every time the head passes center.
  const int32_t step = dir == MOTOR_LEFT ? -dt : dir == MOTOR_RIGHT ? dt : 0;
  posMs += step;
  relMs += step;
  if (centerHit && home != HOME_SEARCHING) {
    foundCenter();
    home = HOME_OK;
  }

  if (!bootHomeChecked && now >= BOOT_HOME_DELAY_MS) {
    bootHomeChecked = true;
    if (s.homeOnBoot && !posKnown) homeRequested = true;
  }
  if (homeRequested) {
    homeRequested = false;
    centering = false;
    startHoming(center);
  }
  if (centerRequested) {
    centerRequested = false;
    gotoActive = false;
    if (!posKnown) {
      if (home != HOME_SEARCHING) startHoming(center);  // homing ends at center too
    } else {
      startCentering(center);
    }
  }

  // Random mode, paused in manual override. It waits for any move in progress,
  // and homes first if the head's position isn't known.
  const bool randomOn = s.random && inputEnabled;
  if (randomOn != randomPrev) {
    randomPrev = randomOn;
    randomDueMs = now + RANDOM_START_MS;
    if (!randomOn && gotoActive) {
      gotoActive = false;
      target = MOTOR_STOP;
      reason = STOP_USER;
    }
    events |= randomOn ? EV_RANDOM_ON : EV_RANDOM_OFF;
  }
  const bool still = dir == MOTOR_STOP && target == MOTOR_STOP && home != HOME_SEARCHING && !centering;
  if (randomOn && still && randomArm) {
    randomArm = false;  // the last change has finished: the pause starts now
    randomDueMs = now + randomDelay(s);
  } else if (randomOn && still && now >= BOOT_HOME_DELAY_MS && (int32_t)(now - randomDueMs) >= 0) {
    if (!posKnown && home == HOME_NONE) startHoming(center);
    else randomStep(center);
    randomArm = true;
  }

  // Console changes act like button presses; a level that just sits there doesn't
  // keep restarting the motor after a stop or fight the web controls. Ignored while homing.
  const uint8_t changed = inputEnabled ? in ^ inputPrev : 0;
  inputPrev = in;
  if ((changed & 0x03) && home != HOME_SEARCHING) {
    const uint8_t m = in & 0x03;
    // Left and right both up: whichever came first keeps going, the other is ignored.
    if (m != 0x03) request(m == 0x01 ? MOTOR_LEFT : m == 0x02 ? MOTOR_RIGHT : MOTOR_STOP, STOP_INPUT);
  }
  if (changed & 0x04) eyes = in & 0x04;

  if (home == HOME_SEARCHING) {
    if (center) {
      foundCenter();
      home = HOME_OK;
      target = MOTOR_STOP;
      reason = STOP_HOMED;
    } else if (returning) {
      if ((dir == MOTOR_LEFT && relMs <= 0) || (dir == MOTOR_RIGHT && relMs >= 0)) {
        home = HOME_FAILED;
        posKnown = false;
        target = MOTOR_STOP;
        reason = STOP_HOME_FAILED;
        events |= EV_HOME_FAILED;
        hasSaved = false;  // the saved position was no help: don't trust it next time
        saveOp = SAVE_CLEAR;
      }
    } else if (directMs) {
      if ((dir == MOTOR_LEFT && relMs <= -directMs) || (dir == MOTOR_RIGHT && relMs >= directMs)) {
        // Not where the saved position said, and not on this side of the start either:
        // go back past the start and search the other side, up to the max swing.
        (dir == MOTOR_LEFT ? leftDone : rightDone) = true;
        ampMs = limitMs;
        directMs = 0;
        target = dir == MOTOR_LEFT ? MOTOR_RIGHT : MOTOR_LEFT;
        events |= EV_SAVED_MISSED;
      }
    } else if ((dir == MOTOR_LEFT && relMs <= -ampMs) || (dir == MOTOR_RIGHT && relMs >= ampMs)) {
      nextSwing();
    }
  } else if (gotoActive) {
    if ((dir == MOTOR_LEFT && posMs <= gotoMs) || (dir == MOTOR_RIGHT && posMs >= gotoMs) ||
        (dir != MOTOR_STOP && dir == limitDir())) {
      gotoActive = false;
      target = MOTOR_STOP;
      reason = STOP_NONE;
    }
  } else if (centering) {
    if (center) {
      centering = false;
      target = MOTOR_STOP;
      reason = STOP_HOMED;
    } else if ((dir == MOTOR_LEFT && posMs <= -centerOverrunMs) || (dir == MOTOR_RIGHT && posMs >= centerOverrunMs)) {
      centering = false;  // went well past where center should be without seeing the switch
      target = MOTOR_STOP;
      reason = STOP_NONE;
      events |= EV_CENTER_MISSED;
    }
  } else if (dir != MOTOR_STOP && !override) {
    if (posKnown && dir == limitDir()) {
      target = MOTOR_STOP;
      reason = STOP_TRAVEL;
    } else if (!posKnown && now - runStartMs >= (uint32_t)limitMs) {
      target = MOTOR_STOP;  // not homed: no move runs longer than one full swing
      reason = STOP_TIMEOUT;
    }
  }

  // Every change passes through off. Reversing also waits out the delay so the
  // motor isn't slammed backwards while it's still spinning.
  if (target != dir) {
    if (dir != MOTOR_STOP) {
      dir = MOTOR_STOP;
      stopMs = now;
    } else if (target == lastMoveDir || now - stopMs >= s.reverseDelayMs) {
      dir = target;
      lastMoveDir = dir;
      runStartMs = now;
      if (!posKnown && home != HOME_SEARCHING && hasSaved) {
        hasSaved = false;  // moving an un-homed head makes the saved position meaningless
        saveOp = SAVE_CLEAR;
      }
      if (home != HOME_SEARCHING) reason = STOP_NONE;
    }
  }

  const bool idle = dir == MOTOR_STOP && target == MOTOR_STOP && home != HOME_SEARCHING;
  if (!idle) idleSince = now;
  if (posKnown && idle && now - idleSince >= SAVE_AFTER_MS && (!hasSaved || abs(posMs - savedPos) >= SAVE_MIN_CHANGE_MS)) {
    hasSaved = true;
    savedPos = posMs;
    saveOp = SAVE_WRITE;
  }

  st = {dir, target, limitDir(), reason, center, closed, override, eyes, home, posKnown ? posMs : 0, centering, randomOn};
  if (dir != MOTOR_STOP || target != MOTOR_STOP) moveEyesUntil = now + EYES_HOLD_MS;  // moving, or about to
  const bool eyesOn = eyes || (s.eyesOnMove && (int32_t)(moveEyesUntil - now) > 0);
  const uint8_t ev = events;
  events = 0;
  const int32_t saved = savedPos;
  portEXIT_CRITICAL(&mux);

  if (ev & EV_HOMING) logInfo("anim", "Homing: swinging up to %u ms each way to find the center switch", s.maxSwingMs);
  if (ev & EV_HOMING_SAVED)
    logInfo("anim", "Homing: last position was %.1f s %s of center, heading %s first", abs(saved) / 1000.0f,
            saved < 0 ? "left" : "right", saved < 0 ? "right" : "left");
  if (ev & EV_SAVED_MISSED)
    logWarn("anim", "Center wasn't where the saved position said; searching the other side");
  if (ev & EV_RANDOM_ON) logInfo("anim", "Random mode on, about every %.1f s", s.randomIntervalMs / 1000.0f);
  if (ev & EV_RANDOM_OFF) logInfo("anim", "Random mode off");
  if (ev & EV_CENTER_MISSED)
    logWarn("anim", "Went past center without the switch closing; position may be off, use Find center");
  if (ev & EV_CENTERED) logInfo("anim", "Head centered: center switch found");
  if (ev & EV_HOME_FAILED) logError("anim", "Homing failed: center switch not found within %u ms either way", s.maxSwingMs);

  uint8_t out = eyesOn ? 0x04 : 0;
  if (dir == MOTOR_LEFT) out |= s.swap ? 0x02 : 0x01;
  if (dir == MOTOR_RIGHT) out |= s.swap ? 0x01 : 0x02;
  return out;
}

bool animMotor(MotorDir want, String &err) {
  portENTER_CRITICAL(&mux);
  const bool ok = request(want, STOP_USER);
  portEXIT_CRITICAL(&mux);
  if (!ok)
    err = String("At the ") + DIR_NAMES[want] + " end of travel. Move the other way first, or turn on Ignore travel limit.";
  return ok;
}

void animCenter() {
  portENTER_CRITICAL(&mux);
  centerRequested = true;
  portEXIT_CRITICAL(&mux);
}

void animHome() {
  portENTER_CRITICAL(&mux);
  homeRequested = true;
  portEXIT_CRITICAL(&mux);
}

void animEyes(bool on) {
  portENTER_CRITICAL(&mux);
  eyes = on;
  portEXIT_CRITICAL(&mux);
}

void animOverride(bool on) {
  portENTER_CRITICAL(&mux);
  override = on;
  portEXIT_CRITICAL(&mux);
}

const char *motorDirName(MotorDir d) { return DIR_NAMES[d]; }
const char *stopReasonName(StopReason r) { return REASON_NAMES[r]; }
const char *homeStateName(HomeState h) { return HOME_NAMES[h]; }
