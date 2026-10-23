#pragma once

#include <stdint.h>

// Values are part of the BLE protocol and must remain stable.
enum LedMode : uint8_t {
  MODE_OFF = 0,
  MODE_EQ = 1,
  MODE_CONSTANT = 2,
  MODE_AEQ = 3,
  MODE_FIREFLY = 4,
  MODE_POWEROFF = 9,
  MODE_BOOTLOADER = 55,
};
