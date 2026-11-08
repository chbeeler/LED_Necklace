#include "button_controller.h"

#include <Arduino.h>

namespace {

constexpr uint8_t PLUS_DRIVE_PIN = D7;
constexpr uint8_t PLUS_SENSE_PIN = D9;
constexpr uint8_t MINUS_DRIVE_PIN = D4;
constexpr uint8_t MINUS_SENSE_PIN = D6;
constexpr uint32_t DEBOUNCE_MS = 25UL;
constexpr uint32_t MODIFIER_LEAD_MS = 100UL;
constexpr uint32_t SECONDARY_TAP_MAX_MS = 600UL;

enum class ChordModifier : uint8_t {
  None,
  Plus,
  Minus,
};

struct ButtonState {
  bool rawPressed = false;
  bool stablePressed = false;
  uint32_t rawChangedMs = 0;
  uint32_t pressedMs = 0;
};

ButtonState plusState;
ButtonState minusState;
bool plusLongHandled = false;
bool minusLongHandled = false;
bool chordActive = false;
bool chordHandled = false;
uint32_t chordStartedMs = 0;
ChordModifier chordModifier = ChordModifier::None;

void updateButton(ButtonState &state, bool rawPressed, uint32_t nowMs,
                  const char *name)
{
  if (rawPressed != state.rawPressed) {
    state.rawPressed = rawPressed;
    state.rawChangedMs = nowMs;
  }

  if (state.stablePressed != state.rawPressed &&
      (uint32_t)(nowMs - state.rawChangedMs) >= DEBOUNCE_MS) {
    state.stablePressed = state.rawPressed;
    if (state.stablePressed) {
      state.pressedMs = nowMs;
      Serial.print(name);
      Serial.println(" pressed");
    }
  }
}

}  // namespace

namespace ButtonController {

void begin()
{
  // Set each output latch before enabling output mode to avoid a LOW glitch.
  digitalWrite(PLUS_DRIVE_PIN, HIGH);
  pinMode(PLUS_DRIVE_PIN, OUTPUT);
  pinMode(PLUS_SENSE_PIN, INPUT_PULLDOWN);

  digitalWrite(MINUS_DRIVE_PIN, HIGH);
  pinMode(MINUS_DRIVE_PIN, OUTPUT);
  pinMode(MINUS_SENSE_PIN, INPUT_PULLDOWN);
}

bool plusPressedRaw()
{
  return digitalRead(PLUS_SENSE_PIN) == HIGH;
}

bool minusPressedRaw()
{
  return digitalRead(MINUS_SENSE_PIN) == HIGH;
}

ButtonAction update(uint32_t nowMs)
{
  const bool plusWasPressed = plusState.stablePressed;
  const bool minusWasPressed = minusState.stablePressed;

  updateButton(plusState, plusPressedRaw(), nowMs, "Plus (D7-D9)");
  updateButton(minusState, minusPressedRaw(), nowMs, "Minus (D4-D6)");

  const bool plusPressed = plusState.stablePressed;
  const bool minusPressed = minusState.stablePressed;

  if (!plusWasPressed && plusPressed) plusLongHandled = false;
  if (!minusWasPressed && minusPressed) minusLongHandled = false;

  if (plusPressed && minusPressed) {
    if (!chordActive) {
      chordActive = true;
      chordHandled = false;
      chordStartedMs = nowMs;
      if (plusWasPressed &&
          (uint32_t)(nowMs - plusState.pressedMs) >= MODIFIER_LEAD_MS) {
        chordModifier = ChordModifier::Plus;
      } else if (minusWasPressed &&
                 (uint32_t)(nowMs - minusState.pressedMs) >=
                     MODIFIER_LEAD_MS) {
        chordModifier = ChordModifier::Minus;
      } else {
        chordModifier = ChordModifier::None;
      }
      plusLongHandled = true;
      minusLongHandled = true;
    } else if (!(plusWasPressed && minusWasPressed)) {
      chordStartedMs = nowMs;
    }

    if (!chordHandled &&
        (uint32_t)(nowMs - chordStartedMs) >= LONG_PRESS_MS) {
      chordHandled = true;
      Serial.println("Both buttons held: system off requested");
      return ButtonAction::RequestSystemOff;
    }
    return ButtonAction::None;
  }

  // Once a chord starts, suppress individual actions until both buttons have
  // been released.
  if (chordActive) {
    const uint32_t overlapMs = nowMs - chordStartedMs;
    if (overlapMs <= SECONDARY_TAP_MAX_MS) {
      if (chordModifier == ChordModifier::Minus && minusPressed &&
          plusWasPressed && !plusPressed) {
        chordHandled = true;
        return ButtonAction::SecondaryUp;
      }
      if (chordModifier == ChordModifier::Plus && plusPressed &&
          minusWasPressed && !minusPressed) {
        chordHandled = true;
        return ButtonAction::SecondaryDown;
      }
    }

    if (!plusPressed && !minusPressed) {
      chordActive = false;
      chordHandled = false;
      chordModifier = ChordModifier::None;
      plusLongHandled = false;
      minusLongHandled = false;
    }
    return ButtonAction::None;
  }

  if (plusPressed && !plusLongHandled &&
      (uint32_t)(nowMs - plusState.pressedMs) >= LONG_PRESS_MS) {
    plusLongHandled = true;
    return ButtonAction::ToggleAeqConstant;
  }

  if (plusWasPressed && !plusPressed) {
    const bool wasLongPress = plusLongHandled;
    plusLongHandled = false;
    if (!wasLongPress) return ButtonAction::BrightnessUp;
  }

  if (minusWasPressed && !minusPressed) {
    const bool wasLongPress = minusLongHandled;
    minusLongHandled = false;
    if (!wasLongPress) return ButtonAction::BrightnessDown;
  }

  return ButtonAction::None;
}

void waitForRelease()
{
  uint32_t releasedSinceMs = 0;
  while (true) {
    if (plusPressedRaw() || minusPressedRaw()) {
      releasedSinceMs = 0;
    } else if (releasedSinceMs == 0) {
      releasedSinceMs = millis();
    } else if ((uint32_t)(millis() - releasedSinceMs) >=
               RELEASE_GUARD_MS) {
      return;
    }
    delay(5);
  }
}

void configureWakeSources()
{
  digitalWrite(PLUS_DRIVE_PIN, HIGH);
  pinMode(PLUS_DRIVE_PIN, OUTPUT);
  digitalWrite(MINUS_DRIVE_PIN, HIGH);
  pinMode(MINUS_DRIVE_PIN, OUTPUT);
  pinMode(PLUS_SENSE_PIN, INPUT_PULLDOWN_SENSE);
  pinMode(MINUS_SENSE_PIN, INPUT_PULLDOWN_SENSE);

  NRF_P0->LATCH = 0xFFFFFFFFUL;
  NRF_P1->LATCH = 0xFFFFFFFFUL;
}

}  // namespace ButtonController
