#include "adaptive_brightness.h"
#if defined(__arm__)
#include "ti_msp_dl_config.h"
#endif

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
#if defined(__arm__)
// TIMA0 ticks at BUSCLK / 8; report equivalent 24 MHz core cycles.
volatile uint32_t aeqPeakCycles[3];  // idle, burst, release
volatile uint32_t aeqTotalCycles;
volatile uint32_t aeqTimedBlocks;
#endif

extern "C" void runAeqFitCheck(void)
{
    brightness.reset();
#if defined(__arm__)
    DL_TimerA_reset(TIMA0);
    DL_TimerA_enablePower(TIMA0);
    delay_cycles(POWER_STARTUP_DELAY);
    DL_TimerA_ClockConfig clock{};
    clock.clockSel = DL_TIMER_CLOCK_BUSCLK;
    clock.divideRatio = DL_TIMER_CLOCK_DIVIDE_8;
    DL_TimerA_setClockConfig(TIMA0, &clock);
    DL_TimerA_TimerConfig timer{};
    timer.period = UINT16_MAX;
    timer.timerMode = DL_TIMER_TIMER_MODE_PERIODIC_UP;
    timer.startTimer = DL_TIMER_STOP;
    DL_TimerA_initTimerMode(TIMA0, &timer);
    DL_TimerA_enableClock(TIMA0);
    DL_TimerA_startCounter(TIMA0);
    for (auto &peak : aeqPeakCycles) peak = 0;
    aeqTotalCycles = 0;
    aeqTimedBlocks = 0;
#endif
    for (unsigned block = 0; block < 96; ++block) {
        const bool burst = block >= 32 && block < 48;
        for (size_t i = 0; i < blockSamples; ++i) {
            pcm[i] = burst ? ((i % 16 < 8) ? 8192 : -8192) : 0;
        }
#if defined(__arm__)
        const uint16_t start = DL_TimerA_getTimerCount(TIMA0);
#endif
        const float output = brightness.process(pcm, blockSamples, sampleRate,
                                                10.0f, 50, 0.060f, 0.04f);
#if defined(__arm__)
        const uint16_t ticks = DL_TimerA_getTimerCount(TIMA0) - start;
        const uint32_t cycles = ticks * 8U;
        const unsigned phase = block < 32 ? 0 : (burst ? 1 : 2);
        if (cycles > aeqPeakCycles[phase]) aeqPeakCycles[phase] = cycles;
        aeqTotalCycles += cycles;
        ++aeqTimedBlocks;
#endif
        if (block == 31) aeqIdleOutput = output;
        if (block == 47) aeqBurstOutput = output;
        if (block == 95) aeqReleaseOutput = output;
    }
#if defined(__arm__)
    DL_TimerA_stopCounter(TIMA0);
    DL_TimerA_disablePower(TIMA0);
#endif
}
