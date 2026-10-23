#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NfcService {

constexpr size_t MAX_URI_LENGTH = 120;

// Loads the saved URI before BLE characteristics are initialized.
void prepare();

// Initializes the Type 2 tag on NFC builds. This is a no-op for normal builds.
void begin();

const char *uri();

// Applies a validated HTTP(S) URI immediately and saves it across restarts.
bool setUri(const char *newUri, size_t length);

// Transfers an NFC-read event from interrupt context into the main loop.
void update(uint32_t nowMs);

bool effectActive();

// Returns true and supplies the current effect brightness while the NFC effect
// is running. At the end of the effect it returns false and clears the effect.
bool renderEffect(uint32_t nowMs, uint16_t &brightness);

}  // namespace NfcService
