#include "adaptive_brightness.h"

namespace {
AdaptiveBrightness liveBrightness;
}

// Adjustable from CCS, in 12-bit ADC counts RMS.
volatile float micNoiseFloor = 3.0f;

extern "C" float updateLedBrightness(float adcRms, uint16_t windowSamples)
{
    // A centered 12-bit sample spans +/-2048 counts, or +/-32768 in PCM16.
    return liveBrightness.processRms(adcRms / 2048.0f, windowSamples / 16000.0f,
                                     micNoiseFloor * 16.0f, 0.060f, 0.04f);
}
