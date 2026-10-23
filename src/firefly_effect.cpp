#include "firefly_effect.h"

#include <Arduino.h>
#include <math.h>

#include "led_output.h"

namespace {

constexpr uint32_t FADE_IN_MS = 350UL;
constexpr uint32_t ON_MS = 20UL;
constexpr uint32_t FADE_OUT_MS = 900UL;
constexpr uint32_t NORMAL_PAUSE_MIN_MS = 1500UL;
constexpr uint32_t NORMAL_PAUSE_MAX_MS = 3500UL;
constexpr uint32_t DOUBLE_GAP_MIN_MS = 350UL;
constexpr uint32_t DOUBLE_GAP_MAX_MS = 700UL;
constexpr uint32_t POST_DOUBLE_PAUSE_MIN_MS = 1800UL;
constexpr uint32_t POST_DOUBLE_PAUSE_MAX_MS = 4000UL;
constexpr uint32_t LONG_PAUSE_MIN_MS = 4000UL;
constexpr uint32_t LONG_PAUSE_MAX_MS = 7000UL;
constexpr long DOUBLE_FLASH_PERCENT = 20;
constexpr long LONG_PAUSE_PERCENT = 10;
constexpr long SECOND_FLASH_MIN_PERCENT = 60;
constexpr long SECOND_FLASH_MAX_PERCENT = 85;
constexpr float GAMMA = 2.2f;

static_assert(FADE_IN_MS > 0 && FADE_OUT_MS > 0,
              "Firefly fade durations must be greater than zero");
static_assert(NORMAL_PAUSE_MIN_MS <= NORMAL_PAUSE_MAX_MS &&
                  DOUBLE_GAP_MIN_MS <= DOUBLE_GAP_MAX_MS &&
                  POST_DOUBLE_PAUSE_MIN_MS <= POST_DOUBLE_PAUSE_MAX_MS &&
                  LONG_PAUSE_MIN_MS <= LONG_PAUSE_MAX_MS,
              "Firefly minimum durations must not exceed their maximums");
static_assert(DOUBLE_FLASH_PERCENT + LONG_PAUSE_PERCENT <= 100,
              "Firefly event probabilities must not exceed 100 percent");
static_assert(SECOND_FLASH_MIN_PERCENT <= SECOND_FLASH_MAX_PERCENT &&
                  SECOND_FLASH_MAX_PERCENT <= 100,
              "Firefly second-flash brightness percentages are invalid");

float smoothstep(float value)
{
  return value * value * (3.0f - 2.0f * value);
}

}  // namespace

void FireflyEffect::reset()
{
  active_ = false;
}

uint32_t FireflyEffect::randomDuration(uint32_t minMs, uint32_t maxMs)
{
  return (uint32_t)random((long)minMs, (long)maxMs + 1L);
}

void FireflyEffect::start(uint32_t nowMs)
{
  active_ = true;
  phase_ = Phase::FadeIn;
  phaseStartMs_ = nowMs;
  phaseDurationMs_ = FADE_IN_MS;
  isSecondFlash_ = false;
  nextFlashIsSecond_ = false;
  peakScale_ = 1.0f;
  nextPeakScale_ = 1.0f;
}

void FireflyEffect::advancePhase()
{
  phaseStartMs_ += phaseDurationMs_;

  switch (phase_) {
    case Phase::FadeIn:
      phase_ = Phase::On;
      phaseDurationMs_ = ON_MS;
      break;

    case Phase::On:
      phase_ = Phase::FadeOut;
      phaseDurationMs_ = FADE_OUT_MS;
      break;

    case Phase::FadeOut: {
      phase_ = Phase::Pause;
      if (isSecondFlash_) {
        nextFlashIsSecond_ = false;
        nextPeakScale_ = 1.0f;
        phaseDurationMs_ = randomDuration(POST_DOUBLE_PAUSE_MIN_MS,
                                          POST_DOUBLE_PAUSE_MAX_MS);
        break;
      }

      const long eventRoll = random(100);
      if (eventRoll < DOUBLE_FLASH_PERCENT) {
        nextFlashIsSecond_ = true;
        nextPeakScale_ = (float)random(SECOND_FLASH_MIN_PERCENT,
                                      SECOND_FLASH_MAX_PERCENT + 1) /
                         100.0f;
        phaseDurationMs_ = randomDuration(DOUBLE_GAP_MIN_MS,
                                          DOUBLE_GAP_MAX_MS);
      } else {
        nextFlashIsSecond_ = false;
        nextPeakScale_ = 1.0f;
        if (eventRoll < DOUBLE_FLASH_PERCENT + LONG_PAUSE_PERCENT) {
          phaseDurationMs_ = randomDuration(LONG_PAUSE_MIN_MS,
                                            LONG_PAUSE_MAX_MS);
        } else {
          phaseDurationMs_ = randomDuration(NORMAL_PAUSE_MIN_MS,
                                            NORMAL_PAUSE_MAX_MS);
        }
      }
      break;
    }

    case Phase::Pause:
      phase_ = Phase::FadeIn;
      phaseDurationMs_ = FADE_IN_MS;
      isSecondFlash_ = nextFlashIsSecond_;
      peakScale_ = nextPeakScale_;
      break;
  }
}

uint16_t FireflyEffect::update(uint32_t nowMs, uint16_t brightness)
{
  if (!active_) start(nowMs);

  while ((uint32_t)(nowMs - phaseStartMs_) >= phaseDurationMs_) {
    advancePhase();
  }

  float level = 0.0f;
  if (phase_ == Phase::On) {
    level = 1.0f;
  } else if (phase_ == Phase::FadeIn || phase_ == Phase::FadeOut) {
    const float progress =
        (float)(nowMs - phaseStartMs_) / (float)phaseDurationMs_;
    const float eased = smoothstep(progress);
    level = phase_ == Phase::FadeIn
                ? sqrtf(eased)
                : 1.0f - powf(eased, 1.5f);
  }

  const float pwmLevel = powf(level, GAMMA);
  uint32_t ledValue =
      (uint32_t)((float)brightness * peakScale_ * pwmLevel + 0.5f);
  if (ledValue > LedOutput::PWM_MAX) ledValue = LedOutput::PWM_MAX;
  return (uint16_t)ledValue;
}
