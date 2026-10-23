#pragma once

#include <stdint.h>

namespace NfcService {

// Initializes the Type 2 tag on NFC builds. This is a no-op for normal builds.
void begin();

// Transfers an NFC-read event from interrupt context into the main loop.
void update(uint32_t nowMs);

bool effectActive();

// Returns true and supplies the current effect brightness while the NFC effect
// is running. At the end of the effect it returns false and clears the effect.
bool renderEffect(uint32_t nowMs, uint16_t &brightness);

}  // namespace NfcService
