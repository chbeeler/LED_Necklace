#include "audio_engine.h"

#include <Arduino.h>
#include <PDM.h>

#include "adaptive_brightness.h"
#include "led_modes.h"
#include "led_output.h"

namespace {

constexpr char CHANNEL_COUNT = 1;
constexpr uint32_t SAMPLE_RATE_HZ = 16000UL;
constexpr uint16_t BUFFER_SAMPLES = 512;
constexpr uint32_t VOLUME_RANGE_WINDOW_MS = 5000UL;
constexpr uint32_t TELEMETRY_WINDOW_MS = 200UL;
constexpr uint16_t PCM_CLIP_THRESHOLD = 32700;

// The ISR fills one buffer while the main loop owns the other. The PDM library
// currently supplies 512-byte blocks; this capacity also tolerates larger
// blocks without changing the hand-off protocol.
int16_t audioBuffers[2][BUFFER_SAMPLES];
volatile int8_t readyBuffer = -1;
volatile uint16_t readySampleCount = 0;
volatile uint8_t writeBuffer = 0;
volatile uint32_t droppedBlocks = 0;

float filteredLoudness = 0.0f;
AdaptiveBrightness adaptiveBrightness;
uint8_t previousLedMode = 0xFF;
uint16_t volumeWindowMin = 0xFFFF;
uint16_t volumeWindowMax = 0;
uint32_t volumeWindowStartMs = 0;
uint16_t telemetryRawPeak = 0;
uint16_t telemetryRms = 0;
uint32_t telemetryClippedSamples = 0;
uint32_t telemetryTotalSamples = 0;
uint16_t telemetrySaturatedBlocks = 0;
uint16_t telemetryAeqBlocks = 0;
uint32_t telemetryWindowStartMs = 0;
uint32_t telemetryPreviousDroppedBlocks = 0;

void onPdmData()
{
  int bytesAvailable = PDM.available();
  if (bytesAvailable <= 0) return;

  const uint8_t destination = writeBuffer;
  const int bufferBytes = (int)sizeof(audioBuffers[destination]);
  if (bytesAvailable > bufferBytes) bytesAvailable = bufferBytes;

  const int bytesRead =
      PDM.read(audioBuffers[destination], (size_t)bytesAvailable);
  if (bytesRead <= 0) return;

  if (readyBuffer < 0) {
    readySampleCount = (uint16_t)(bytesRead / (int)sizeof(int16_t));
    readyBuffer = (int8_t)destination;
    writeBuffer = destination ^ 1U;
  } else {
    ++droppedBlocks;
  }
}

bool takeBlock(uint8_t &bufferIndex, uint16_t &sampleCount)
{
  noInterrupts();
  if (readyBuffer < 0) {
    interrupts();
    return false;
  }

  bufferIndex = (uint8_t)readyBuffer;
  sampleCount = readySampleCount;
  readyBuffer = -1;
  readySampleCount = 0;
  interrupts();
  return true;
}

uint16_t clampLoudness(float value)
{
  if (value <= 0.0f) return 0;
  if (value >= 65535.0f) return 65535;
  return (uint16_t)(value + 0.5f);
}

uint16_t normalizedToU16(float value)
{
  if (value <= 0.0f) return 0;
  if (value >= 1.0f) return 32767;
  return (uint16_t)(value * 32768.0f + 0.5f);
}

uint16_t clampCounter(uint32_t value)
{
  return value > 65535UL ? 65535 : (uint16_t)value;
}

}  // namespace

namespace AudioEngine {

bool begin()
{
  PDM.onReceive(onPdmData);
  return PDM.begin(CHANNEL_COUNT, (int)SAMPLE_RATE_HZ);
}

AudioResult update(const AudioSettings &settings, bool processBlock,
                   uint32_t nowMs)
{
  AudioResult result;

  if (settings.ledMode != previousLedMode) {
    if (settings.ledMode == MODE_AEQ) adaptiveBrightness.reset();
    previousLedMode = settings.ledMode;
  }

  uint8_t bufferIndex = 0;
  uint16_t sampleCount = 0;
  if (!takeBlock(bufferIndex, sampleCount) || !processBlock) return result;

  const int16_t *samples = audioBuffers[bufferIndex];
  int32_t sumAbs = 0;
  uint64_t sumSquares = 0;
  uint16_t rawPeak = 0;
  uint16_t clippedSamples = 0;
  for (uint16_t i = 0; i < sampleCount; ++i) {
    int32_t sample = samples[i];
    int32_t absoluteSample = sample < 0 ? -sample : sample;
    if (absoluteSample > 32767) absoluteSample = 32767;
    sumAbs += absoluteSample;
    sumSquares += (uint64_t)((int64_t)sample * (int64_t)sample);
    if ((uint16_t)absoluteSample > rawPeak) rawPeak = (uint16_t)absoluteSample;
    if (absoluteSample >= PCM_CLIP_THRESHOLD) ++clippedSamples;
  }

  const float averageLoudness = sampleCount > 0
                                    ? (float)sumAbs / (float)sampleCount
                                    : 0.0f;
  const float rms = sampleCount > 0
                        ? sqrtf((float)sumSquares / (float)sampleCount) /
                              32768.0f
                        : 0.0f;
  filteredLoudness +=
      settings.filterAlpha * (averageLoudness - filteredLoudness);

  const uint16_t loudness = clampLoudness(filteredLoudness);
  if (volumeWindowStartMs == 0) volumeWindowStartMs = nowMs;
  if (loudness < volumeWindowMin) volumeWindowMin = loudness;
  if (loudness > volumeWindowMax) volumeWindowMax = loudness;

  if ((uint32_t)(nowMs - volumeWindowStartMs) >= VOLUME_RANGE_WINDOW_MS) {
    result.volumeRangeReady = true;
    result.volumeMin = volumeWindowMin == 0xFFFF ? 0 : volumeWindowMin;
    result.volumeMax = volumeWindowMax;
    volumeWindowStartMs = nowMs;
    volumeWindowMin = 0xFFFF;
    volumeWindowMax = 0;
  }

  float ledValue = 0.0f;
  bool aeqSaturated = false;
  if (settings.ledMode == MODE_AEQ) {
    const float adaptiveOutput = adaptiveBrightness.process(
        samples, sampleCount, SAMPLE_RATE_HZ, settings.noiseFloor,
        (float)settings.aeqReleaseMs / 1000.0f,
        (float)settings.aeqIdlePercent / 100.0f);
    const uint16_t brightnessLimit =
        settings.brightness < LedOutput::PWM_MAX
            ? settings.brightness
            : LedOutput::PWM_MAX;
    ledValue = adaptiveOutput * (float)brightnessLimit;
    aeqSaturated = adaptiveBrightness.saturated() ||
                   adaptiveBrightness.output() >= 0.995f;
  } else {
    float effectiveLoudness = filteredLoudness - settings.noiseFloor;
    if (effectiveLoudness < 0.0f) effectiveLoudness = 0.0f;
    ledValue = effectiveLoudness * settings.sensitivity *
               (float)settings.brightness / 100.0f;
  }

  if (ledValue > (float)LedOutput::PWM_MAX) {
    ledValue = (float)LedOutput::PWM_MAX;
  }

  uint16_t output = (uint16_t)ledValue;
  if (output > settings.brightness) output = settings.brightness;

  switch (settings.ledMode) {
    case MODE_OFF:
    case MODE_POWEROFF:
      output = 0;
      break;
    case MODE_CONSTANT:
      output = settings.brightness;
      break;
    default:
      break;
  }

  if (output > LedOutput::PWM_MAX) output = LedOutput::PWM_MAX;
  if (nowMs > 500UL) {
    result.ledValueReady = true;
    result.ledValue = output;
  }

  if (telemetryWindowStartMs == 0) telemetryWindowStartMs = nowMs;
  if (rawPeak > telemetryRawPeak) telemetryRawPeak = rawPeak;
  telemetryRms = normalizedToU16(rms);
  telemetryClippedSamples += clippedSamples;
  telemetryTotalSamples += sampleCount;
  if (settings.ledMode == MODE_AEQ) {
    ++telemetryAeqBlocks;
    if (aeqSaturated) ++telemetrySaturatedBlocks;
  }

  if ((uint32_t)(nowMs - telemetryWindowStartMs) >= TELEMETRY_WINDOW_MS) {
    const uint32_t currentDroppedBlocks = droppedBlockCount();
    result.telemetryReady = true;
    result.telemetry.rawPeak = telemetryRawPeak;
    result.telemetry.rms = telemetryRms;
    if (telemetryAeqBlocks > 0) {
      result.telemetry.envelope = normalizedToU16(adaptiveBrightness.envelope());
      result.telemetry.floor = normalizedToU16(adaptiveBrightness.floor());
      result.telemetry.ceiling = normalizedToU16(adaptiveBrightness.ceiling());
      result.telemetry.output = normalizedToU16(adaptiveBrightness.output());
    }
    result.telemetry.clippedSamples = clampCounter(telemetryClippedSamples);
    result.telemetry.totalSamples = clampCounter(telemetryTotalSamples);
    result.telemetry.saturatedBlocks =
        telemetrySaturatedBlocks > 255 ? 255 : (uint8_t)telemetrySaturatedBlocks;
    result.telemetry.totalAeqBlocks =
        telemetryAeqBlocks > 255 ? 255 : (uint8_t)telemetryAeqBlocks;
    result.telemetry.droppedBlocks = clampCounter(
        currentDroppedBlocks - telemetryPreviousDroppedBlocks);

    telemetryRawPeak = 0;
    telemetryClippedSamples = 0;
    telemetryTotalSamples = 0;
    telemetrySaturatedBlocks = 0;
    telemetryAeqBlocks = 0;
    telemetryWindowStartMs = nowMs;
    telemetryPreviousDroppedBlocks = currentDroppedBlocks;
  }
  return result;
}

uint32_t droppedBlockCount()
{
  noInterrupts();
  const uint32_t count = droppedBlocks;
  interrupts();
  return count;
}

}  // namespace AudioEngine
