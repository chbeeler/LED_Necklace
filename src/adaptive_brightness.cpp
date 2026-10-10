#include "adaptive_brightness.h"

#include <math.h>

AdaptiveBrightness::AdaptiveBrightness()
{
  reset();
}

void AdaptiveBrightness::reset()
{
  envelope_ = 0.0f;
  floor_ = 0.001f;
  ceiling_ = 0.020f;
  previousLevel_ = 0.0f;
  flash_ = 0.0f;
  output_ = 0.0f;
}

float AdaptiveBrightness::smoothingCoefficient(float seconds, float dt)
{
  if (seconds < 0.001f) seconds = 0.001f;
  return 1.0f - expf(-dt / seconds);
}

float AdaptiveBrightness::clamp01(float value)
{
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

float AdaptiveBrightness::process(const int16_t* samples,
                                  size_t sampleCount,
                                  uint32_t sampleRate,
                                  float sensitivity)
{
  if (samples == nullptr || sampleCount == 0 || sampleRate == 0) {
    return output_;
  }

  const float pcmScale = 1.0f / 32768.0f;
  float sumSquares = 0.0f;
  for (size_t i = 0; i < sampleCount; ++i) {
    const float sample = (float)samples[i] * pcmScale;
    sumSquares += sample * sample;
  }

  const float dt = (float)sampleCount / (float)sampleRate;
  const float rms = sqrtf(sumSquares / (float)sampleCount + 1.0e-12f);

  const float envelopeTau = rms > envelope_ ? 0.025f : 0.180f;
  envelope_ += smoothingCoefficient(envelopeTau, dt) * (rms - envelope_);

  const float floorTau = rms < floor_ ? 1.5f : 18.0f;
  if (rms < ceiling_ * 0.35f) {
    floor_ += smoothingCoefficient(floorTau, dt) * (rms - floor_);
  }

  float ceilingTarget = envelope_;
  const float minimumCeiling = floor_ + 0.004f;
  if (ceilingTarget < minimumCeiling) ceilingTarget = minimumCeiling;
  const float ceilingTau = ceilingTarget > ceiling_ ? 0.35f : 6.0f;
  ceiling_ += smoothingCoefficient(ceilingTau, dt) * (ceilingTarget - ceiling_);

  float span = ceiling_ - floor_;
  if (span < 0.003f) span = 0.003f;
  float level = clamp01((envelope_ - floor_) / span);
  level = clamp01(level * sensitivity);

  float onset = level - previousLevel_;
  if (onset < 0.0f) onset = 0.0f;
  previousLevel_ = level;

  const float decayedFlash = flash_ * expf(-dt / 0.13f);
  float onsetFlash = onset * 3.2f;
  if (onsetFlash > 1.0f) onsetFlash = 1.0f;
  flash_ = decayedFlash > onsetFlash ? decayedFlash : onsetFlash;

  const float shapedLevel = powf(level, 1.35f);
  const float target = shapedLevel > flash_ ? shapedLevel : flash_;
  const float outputTau = target > output_ ? 0.018f : 0.110f;
  output_ += smoothingCoefficient(outputTau, dt) * (target - output_);
  output_ = clamp01(output_);
  return output_;
}
