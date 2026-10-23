#pragma once

#include <stdint.h>

class FireflyEffect {
 public:
  void reset();
  uint16_t update(uint32_t nowMs, uint16_t brightness);

 private:
  enum class Phase : uint8_t {
    FadeIn,
    On,
    FadeOut,
    Pause,
  };

  void start(uint32_t nowMs);
  void advancePhase();
  static uint32_t randomDuration(uint32_t minMs, uint32_t maxMs);

  Phase phase_ = Phase::FadeIn;
  uint32_t phaseStartMs_ = 0;
  uint32_t phaseDurationMs_ = 0;
  bool active_ = false;
  bool isSecondFlash_ = false;
  bool nextFlashIsSecond_ = false;
  float peakScale_ = 1.0f;
  float nextPeakScale_ = 1.0f;
};
