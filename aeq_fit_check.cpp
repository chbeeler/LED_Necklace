#include "adaptive_brightness.h"

namespace {
constexpr uint32_t sampleRate = 16000;
constexpr size_t blockSamples = 64;  // 4 ms at 16 kHz
AdaptiveBrightness brightness;
int16_t pcm[blockSamples];
}

// Watch these in CCS after the timer example reaches its breakpoint.
volatile float aeqIdleOutput;
volatile float aeqBurstOutput;
volatile float aeqReleaseOutput;

extern "C" void runAeqFitCheck(void)
{
    brightness.reset();
    for (unsigned block = 0; block < 96; ++block) {
        const bool burst = block >= 32 && block < 48;
        for (size_t i = 0; i < blockSamples; ++i) {
            pcm[i] = burst ? ((i % 16 < 8) ? 8192 : -8192) : 0;
        }
        const float output = brightness.process(pcm, blockSamples, sampleRate,
                                                10.0f, 0.060f, 0.04f);
        if (block == 31) aeqIdleOutput = output;
        if (block == 47) aeqBurstOutput = output;
        if (block == 95) aeqReleaseOutput = output;
    }
}
