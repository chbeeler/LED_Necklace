#include "adaptive_brightness.h"

namespace {
AdaptiveBrightness liveBrightness;
}

// Quiet-room cutoff in 12-bit ADC counts RMS; both settings are editable in CCS.
volatile float micNoiseFloor = 1.2f;
volatile float micInputGain = 12.0f;

extern "C" float updateLedBrightness(float adcRms, uint16_t windowSamples)
{
    // A centered 12-bit sample spans +/-2048 counts, or +/-32768 in PCM16.
    const float aboveNoise = adcRms - micNoiseFloor;
    const float signal = aboveNoise > 0.0f ? aboveNoise * micInputGain : 0.0f;
    return liveBrightness.processRms(signal / 2048.0f, windowSamples / 16000.0f,
                                     10.0f, 0.060f, 0.04f);
}
