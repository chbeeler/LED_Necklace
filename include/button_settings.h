#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ButtonSettings {

constexpr uint16_t DEFAULT_BRIGHTNESS = 100;

inline uint16_t stepBrightness(uint16_t current, bool increase)
{
  static constexpr uint16_t BRIGHTNESS_LEVELS[] = {
    0, 5, 10, 20, 30, 50, 70, 100, 150, 250, 400
  };
  static constexpr size_t BRIGHTNESS_LEVEL_COUNT =
      sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);
  uint16_t next = current;

  if (increase) {
    for (size_t i = 0; i < BRIGHTNESS_LEVEL_COUNT; ++i) {
      if (BRIGHTNESS_LEVELS[i] > current) {
        next = BRIGHTNESS_LEVELS[i];
        break;
      }
    }
  } else {
    for (size_t i = BRIGHTNESS_LEVEL_COUNT; i > 0; --i) {
      if (BRIGHTNESS_LEVELS[i - 1] < current) {
        next = BRIGHTNESS_LEVELS[i - 1];
        break;
      }
    }
  }

  return next;
}

inline uint8_t stepIdlePercent(uint8_t current, bool increase)
{
  if (increase) return current < 10 ? current + 1 : 10;
  return current > 0 ? current - 1 : 0;
}

inline uint16_t fineBrightness(uint16_t current, bool increase)
{
  if (increase) return current < 4095 ? current + 1 : 4095;
  return current > 0 ? current - 1 : 0;
}

inline uint8_t stepAeqSensitivity(uint8_t current, bool increase, bool fine)
{
  const uint8_t step = fine ? 1 : 5;
  if (increase) return current <= 100 - step ? current + step : 100;
  return current >= step ? current - step : 0;
}

inline uint16_t stepAeqReleaseMs(uint16_t current, bool increase, bool fine)
{
  const uint16_t step = fine ? 1 : 10;
  if (increase) return current <= 140 - step ? current + step : 140;
  return current >= 20 + step ? current - step : 20;
}

}  // namespace ButtonSettings
