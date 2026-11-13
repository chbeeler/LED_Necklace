#include "adaptive_brightness.h"

#include <math.h>

AdaptiveBrightness::AdaptiveBrightness()
{
  reset();
}

void AdaptiveBrightness::reset()
{
  envelope_ = 0.0f;
  floor_ = 0.0f;
  ceiling_ = 0.0f;
  baseline_ = 0.0f;
  modulationPeak_ = 0.0f;
  output_ = 0.0f;
  saturated_ = false;
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
                                  float noiseFloor,
                                  uint8_t sensitivity,
                                  float releaseSeconds,
                                  float idleLevel)
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

  return processRms(rms, dt, noiseFloor, sensitivity, releaseSeconds,
                    idleLevel);
}

float AdaptiveBrightness::processRms(float rms, float dt, float noiseFloor,
                                     uint8_t sensitivity,
                                     float releaseSeconds, float idleLevel)
{
  if (!(dt > 0.0f) || !(rms >= 0.0f)) return output_;
  const float pcmScale = 1.0f / 32768.0f;
  if (releaseSeconds < 0.020f) releaseSeconds = 0.020f;
  if (releaseSeconds > 0.140f) releaseSeconds = 0.140f;

  // Follow musical attacks quickly, but retain enough release time for a
  // visible pulse rather than tracking individual waveform cycles.
  const float envelopeTau = rms > envelope_ ? 0.020f : releaseSeconds;
  envelope_ += smoothingCoefficient(envelopeTau, dt) * (rms - envelope_);

  // The slow envelope represents the current music level. Subtracting it
  // makes the response depend on rhythmic movement, not absolute venue volume.
  const float baselineTau = envelope_ > baseline_ ? 1.20f : 2.00f;
  baseline_ += smoothingCoefficient(baselineTau, dt) *
               (envelope_ - baseline_);

  float modulation = envelope_ - baseline_;
  if (modulation < 0.0f) modulation = 0.0f;

  // Normalize the rhythmic movement against recent strong movements. The peak
  // falls slowly enough to avoid amplifying every small gap or noise transient.
  const float decayedModulationPeak =
      modulationPeak_ * expf(-dt / 2.0f);
  modulationPeak_ = modulation > decayedModulationPeak
                        ? modulation
                        : decayedModulationPeak;

  // Keep tiny microphone/noise changes from being expanded to a full flash.
  const float normalizedNoiseFloor =
      noiseFloor > 0.0f ? noiseFloor * pcmScale : 0.0f;
  float minimumModulation = normalizedNoiseFloor * 2.0f;
  if (minimumModulation < 0.0005f) minimumModulation = 0.0005f;
  if (sensitivity > 100) sensitivity = 100;
  // 50 preserves the original gate. Each half-range changes the threshold by
  // a factor of four, giving useful control over quiet rooms and noisy venues
  // without changing the adaptive beat normalization itself.
  const float sensitivityScale =
      powf(4.0f, (50.0f - (float)sensitivity) / 50.0f);
  minimumModulation *= sensitivityScale;
  const bool beatActive = envelope_ > normalizedNoiseFloor &&
                          modulationPeak_ > minimumModulation;

  float beat = 0.0f;
  if (beatActive) {
    beat = clamp01(modulation / modulationPeak_);
  }

  const float betweenBeatLevel = clamp01(idleLevel);
  const float shapedBeat = beat * sqrtf(beat);
  // AEQ always retains its between-beat level. The signal gate suppresses
  // false pulses in quiet conditions, but never switches the baseline off.
  const float target =
      betweenBeatLevel + (1.0f - betweenBeatLevel) * shapedBeat;

  floor_ = baseline_;
  ceiling_ = baseline_ + modulationPeak_;
  saturated_ = beatActive && target >= 0.995f;

  const float outputTau = target > output_ ? 0.018f : releaseSeconds;
  output_ += smoothingCoefficient(outputTau, dt) * (target - output_);
  output_ = clamp01(output_);
  return output_;
}
