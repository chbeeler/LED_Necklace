#pragma once

#include <stddef.h>
#include <stdint.h>

class AdaptiveBrightness
{
public:
  AdaptiveBrightness();

  void reset();
  float process(const int16_t* samples,
                size_t sampleCount,
                uint32_t sampleRate,
                float sensitivity);

  float envelope() const { return envelope_; }
  float floor() const { return floor_; }
  float ceiling() const { return ceiling_; }
  float output() const { return output_; }
  bool saturated() const { return saturated_; }

private:
  static float smoothingCoefficient(float seconds, float dt);
  static float clamp01(float value);

  float envelope_;
  float floor_;
  float ceiling_;
  float previousLevel_;
  float flash_;
  float output_;
  bool saturated_;
};
