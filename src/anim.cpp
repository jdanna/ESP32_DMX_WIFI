#include "anim.h"

#include "board.h"

namespace {

constexpr uint32_t DEBOUNCE_MS = 20;

const char *DIR_NAMES[] = {"stop", "left", "right"};
const char *REASON_NAMES[] = {nullptr, "user", "limit", "timeout", "input"};

// Shared between the engine task and the web handlers.
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
MotorDir target = MOTOR_STOP, atLimit = MOTOR_STOP;
StopReason reason = STOP_NONE;
bool override = false, eyes = false;

// Owned by the engine task.
MotorDir dir = MOTOR_STOP, lastMoveDir = MOTOR_STOP;
uint32_t stopMs, runStartMs, rawSince;
bool rawPrev, closed, limitPrev;
uint8_t inputPrev;

// Caller holds mux.
bool request(MotorDir want, StopReason why) {
  if (want != MOTOR_STOP && want == atLimit && !override) return false;
  target = want;
  if (want == MOTOR_STOP) reason = why;
  return true;
}

}  // namespace

void animBegin() {
  pinMode(PIN_DI1, INPUT_PULLUP);
  rawPrev = closed = limitPrev = digitalRead(PIN_DI1) == LOW;
}

uint8_t animUpdate(uint32_t now, const AnimSettings &s, uint8_t inputMask, bool inputEnabled, AnimStatus &st) {
  const bool raw = digitalRead(PIN_DI1) == LOW;
  if (raw != rawPrev) {
    rawPrev = raw;
    rawSince = now;
  }
  if (now - rawSince >= DEBOUNCE_MS) closed = raw;
  const bool limit = closed != s.limitNc;
  const bool limitHit = limit && !limitPrev;  // only a fresh hit counts, so it can drive off a held switch
  limitPrev = limit;

  const uint8_t in = inputMask & ANIM_RELAY_MASK;

  portENTER_CRITICAL(&mux);
  // Console changes act like button presses; a level that just sits there doesn't
  // keep restarting the motor after a limit stop or fight the web controls.
  const uint8_t changed = inputEnabled ? in ^ inputPrev : 0;
  inputPrev = in;
  if (changed & 0x03) {
    const uint8_t m = in & 0x03;
    // Left and right both up: whichever came first keeps going, the other is ignored.
    if (m != 0x03) request(m == 0x01 ? MOTOR_LEFT : m == 0x02 ? MOTOR_RIGHT : MOTOR_STOP, STOP_INPUT);
  }
  if (changed & 0x04) eyes = in & 0x04;

  if (dir != MOTOR_STOP) {
    if (limitHit && !override && now - runStartMs >= s.limitIgnoreMs) {
      atLimit = dir;
      target = MOTOR_STOP;
      reason = STOP_LIMIT;
    } else if (s.maxRunS && now - runStartMs >= s.maxRunS * 1000UL) {
      target = MOTOR_STOP;
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
      reason = STOP_NONE;
      if (atLimit != dir) atLimit = MOTOR_STOP;  // heading away from the limit clears it
    }
  }

  st = {dir, target, atLimit, reason, limit, closed, override, eyes};
  const bool eyesOn = eyes;
  portEXIT_CRITICAL(&mux);

  uint8_t out = eyesOn ? 0x04 : 0;
  if (dir == MOTOR_LEFT) out |= s.swap ? 0x02 : 0x01;
  if (dir == MOTOR_RIGHT) out |= s.swap ? 0x01 : 0x02;
  return out;
}

bool animMotor(MotorDir want, String &err) {
  portENTER_CRITICAL(&mux);
  const bool ok = request(want, STOP_USER);
  portEXIT_CRITICAL(&mux);
  if (!ok) err = String("Already at the ") + DIR_NAMES[want] + " limit. Run the other way first, or turn on the limit override.";
  return ok;
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
