#include "button_gestures.h"

namespace {
constexpr uint32_t DEBOUNCE_MS = 25UL;
constexpr uint32_t MODIFIER_LEAD_MS = 100UL;
constexpr uint32_t SECONDARY_TAP_MAX_MS = 600UL;
}  // namespace

void ButtonGestures::updateButton(ButtonState &state, bool rawPressed, uint32_t nowMs)
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
    }
  }
}

ButtonAction ButtonGestures::update(bool plusRaw, bool minusRaw, uint32_t nowMs)
{
  const bool plusWasPressed = plusState.stablePressed;
  const bool minusWasPressed = minusState.stablePressed;

  updateButton(plusState, plusRaw, nowMs);
  updateButton(minusState, minusRaw, nowMs);

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
