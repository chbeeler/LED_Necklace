#pragma once

#include <stdint.h>

struct AudioSettings {
  uint8_t ledMode;
  uint16_t brightness;
  float sensitivity;
  float filterAlpha;
  float noiseFloor;
};

struct AudioResult {
  bool ledValueReady = false;
  uint16_t ledValue = 0;
  bool volumeRangeReady = false;
  uint16_t volumeMin = 0;
  uint16_t volumeMax = 0;
};

namespace AudioEngine {

bool begin();

// Consumes at most one completed PDM block. Set processBlock=false when an
// effect owns the LEDs; the block is still released so capture can continue.
AudioResult update(const AudioSettings &settings, bool processBlock,
                   uint32_t nowMs);

uint32_t droppedBlockCount();

}  // namespace AudioEngine
