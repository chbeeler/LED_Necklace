#pragma once

#include <stdint.h>

enum class ButtonAction : uint8_t {
  None,
  BrightnessUp,
  BrightnessDown,
  SecondaryUp,
  SecondaryDown,
  ToggleAeqConstant,
  RequestSystemOff,
};

class ButtonGestures {
 public:
  static constexpr uint32_t LONG_PRESS_MS = 1500;
  ButtonAction update(bool plusRaw, bool minusRaw, uint32_t nowMs);
  bool plusPressed() const { return plusState.stablePressed; }
  bool minusPressed() const { return minusState.stablePressed; }

 private:
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
  static void updateButton(ButtonState &state, bool rawPressed, uint32_t nowMs);
};
