#pragma once

#include <stdint.h>

struct AudioSettings {
  uint8_t ledMode;
  uint16_t brightness;
  float sensitivity;
  float filterAlpha;
  float noiseFloor;
  uint8_t aeqSensitivity;
  uint16_t aeqReleaseMs;
  uint8_t aeqIdlePercent;
};

struct AudioTelemetry {
  uint16_t rawPeak = 0;
  uint16_t rms = 0;
  uint16_t envelope = 0;
  uint16_t floor = 0;
  uint16_t ceiling = 0;
  uint16_t output = 0;
  uint16_t clippedSamples = 0;
  uint16_t totalSamples = 0;
  uint8_t saturatedBlocks = 0;
  uint8_t totalAeqBlocks = 0;
  uint16_t droppedBlocks = 0;
};

struct AudioResult {
  bool ledValueReady = false;
  uint16_t ledValue = 0;
  bool volumeRangeReady = false;
  uint16_t volumeMin = 0;
  uint16_t volumeMax = 0;
  bool telemetryReady = false;
  AudioTelemetry telemetry;
};

namespace AudioEngine {

bool begin();

// Consumes at most one completed PDM block. Set processBlock=false when an
// effect owns the LEDs; the block is still released so capture can continue.
AudioResult update(const AudioSettings &settings, bool processBlock,
                   uint32_t nowMs);

uint32_t droppedBlockCount();

}  // namespace AudioEngine
