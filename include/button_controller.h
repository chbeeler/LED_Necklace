#pragma once

#include "button_gestures.h"

namespace ButtonController {

constexpr uint32_t LONG_PRESS_MS = ButtonGestures::LONG_PRESS_MS;
constexpr uint32_t RELEASE_GUARD_MS = 100UL;

void begin();
ButtonAction update(uint32_t nowMs);

bool plusPressedRaw();
bool minusPressedRaw();
void waitForRelease();
void configureWakeSources();

}  // namespace ButtonController
