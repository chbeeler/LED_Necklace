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

namespace ButtonController {

constexpr uint32_t LONG_PRESS_MS = 1500UL;
constexpr uint32_t RELEASE_GUARD_MS = 100UL;

void begin();
ButtonAction update(uint32_t nowMs);

bool plusPressedRaw();
bool minusPressedRaw();
void waitForRelease();
void configureWakeSources();

}  // namespace ButtonController
