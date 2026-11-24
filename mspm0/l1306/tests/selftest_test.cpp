// Host test: the self-test's gain-independent tracker must agree with the real
// AEQ algorithm at every gain, and its interval counts must match what the LED
// would have done.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "adaptive_brightness.h"
#include "selftest.h"

// Board and audio stubs.
static uint32_t flash[128];
static unsigned stores, layoutCalls;
static bool gpamp, testMode;
extern "C" {
void boardGpampEnable(bool e) { gpamp = e; }
void boardSelectTestLayout(void) { ++layoutCalls; }
void audioSetTestMode(bool e) { testMode = e; }
const uint32_t *boardStoredRecord(void) { return flash; }
bool boardStoreRecord(const uint32_t *words, unsigned count)
{
    assert(count % 2 == 0 && count <= 128);
    memcpy(flash, words, 4 * count);
    ++stores;
    return true;
}
}

static const float DT = 256.0f / 16000.0f, IDLE = 0.04f, RELEASE = 0.060f;
// LED duty 100 of 1600 (idle is 64): what the earlier UART test called a flash.
static const float FLASH_OUTPUT = 100.0f / 1600.0f;

// Window RMS in ADC counts: steady noise with slow drift and occasional bumps.
static std::vector<float> makeRms(unsigned seed, unsigned windows)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 0.06f);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    std::vector<float> rms(windows);
    float bump = 0.0f;
    for (unsigned i = 0; i < windows; ++i) {
        if (uniform(rng) < 0.004f) bump = 0.2f + 2.0f * uniform(rng);
        bump *= 0.8f;
        rms[i] = std::fabs(1.3f + 0.1f * std::sin(i * 0.01f) + noise(rng) + bump);
    }
    return rms;
}

int main()
{
    const std::vector<float> rms = makeRms(1, 62 * 600);

    // 1. Tracker versus the real algorithm, window by window, at several gains.
    for (float gain : {2.5f, 5.0f, 9.0f, 14.0f, 22.0f, 40.0f, 90.0f}) {
        AdaptiveBrightness real;
        SelftestTracker tracker = {};
        float output = 0.0f;
        unsigned mismatches = 0;
        for (float r : rms) {
            const float unit = selftestUnitSignal(r);
            real.processRms(unit * gain, DT, 10.0f, 100, RELEASE, IDLE);
            float needed;
            const float beat = selftestTrack(&tracker, unit, &needed);
            const float shaped = gain > needed ? beat * std::sqrt(beat) : 0.0f;
            const float target = IDLE + (1.0f - IDLE) * shaped;
            const float tau = target > output ? 0.018f : RELEASE;
            output += (1.0f - std::exp(-DT / tau)) * (target - output);
            if (std::fabs(output - real.output()) > 2e-3f) ++mismatches;
        }
        assert(mismatches == 0);
    }
    puts("PASS: tracker reproduces the AEQ output at every gain");

    // 2. Gain steps.
    assert(selftestBinGain(0) == 2.0f);
    assert(std::fabs(selftestBinGain(4) - 4.0f) < 1e-4f);
    assert(std::fabs(selftestBinGain(20) - 64.0f) < 1e-2f);
    assert(selftestBin(1.9f) == 0 && selftestBin(2.0f) == 1);
    assert(selftestBin(63.0f) == 20 && selftestBin(1e9f) == SELFTEST_BINS);
    assert(selftestBin(INFINITY) == SELFTEST_BINS);

    // 3. Whole test: six paths with different noise, compared per gain step
    //    with LED flashes of the real algorithm over the same intervals.
    assert(selftestResults() == nullptr); // Nothing stored yet.
    selftestStart();
    assert(selftestActive() && gpamp && testMode && layoutCalls == 1);
    assert(selftestLedDuty() == 800);
    std::vector<std::vector<float>> paths;
    for (unsigned p = 0; p < SELFTEST_PATHS; ++p) {
        paths.push_back(makeRms(10 + p, SELFTEST_SETTLE_WINDOWS + 64 * 300));
        for (float &r : paths.back()) r *= 0.5f + 0.25f * p;
    }
    const unsigned total = paths[0].size();
    for (unsigned w = 0; w < total; ++w) {
        float levels[SELFTEST_PATHS];
        for (unsigned p = 0; p < SELFTEST_PATHS; ++p) levels[p] = paths[p][w];
        selftestWindow(levels);
    }
    assert(selftestLedDuty() == 64);
    const SelftestRecord *result = selftestResults();
    // Each save drops the next four windows, so slightly fewer than 300 fit.
    assert(result && result->intervals >= 298 && result->intervals <= 300);
    assert(stores == 2); // After 128 and 256 intervals.

    unsigned compared = 0, close = 0, informative = 0;
    for (unsigned p = 0; p < SELFTEST_PATHS; ++p) {
        const float mean = result->meanRmsMilli[p] / 1000.0f;
        assert(std::fabs(mean - 1.3f * (0.5f + 0.25f * p)) < 0.1f * (0.5f + 0.25f * p));
        for (unsigned bin = 0; bin < SELFTEST_BINS; ++bin) {
            if (bin) assert(result->flashIntervals[p][bin] >= result->flashIntervals[p][bin - 1]);
            assert(result->flashIntervals[p][bin] <= result->intervals);
            // Reference: run the real algorithm at this gain. Saves shift the
            // interval grid by a few windows, so compare counts, not intervals.
            AdaptiveBrightness real;
            const float gain = selftestBinGain(bin);
            unsigned flashes = 0, inInterval = 0;
            bool flashed = false;
            for (unsigned w = 0; w < total; ++w) {
                const float out = real.processRms(selftestUnitSignal(paths[p][w]) * gain,
                                                  DT, 10.0f, 100, RELEASE, IDLE);
                if (w < SELFTEST_SETTLE_WINDOWS) continue;
                if (out > FLASH_OUTPUT) flashed = true;
                if (++inInterval == SELFTEST_INTERVAL_WINDOWS) {
                    flashes += flashed; flashed = false; inInterval = 0;
                }
            }
            const int counted = result->flashIntervals[p][bin];
            ++compared;
            if (flashes > 10 && flashes + 10 < result->intervals) ++informative;
            if (std::abs(counted - (int)flashes) <= 3 + (int)flashes / 5) ++close;
            else printf("path %u gain %.1f: counted %d, LED flashed in %u\n", p, gain, counted, flashes);
        }
    }
    printf("interval counts within tolerance of the LED: %u of %u\n", close, compared);
    assert(close == compared);
    // The comparison must cover gains that are neither silent nor saturated.
    printf("of those, neither silent nor always flashing: %u\n", informative);
    assert(informative >= 20);
    puts("PASS: interval counts match LED flashes of the real algorithm");

    // 4. Stop stores the result; it is then served from flash.
    selftestStop();
    assert(!selftestActive() && !testMode && stores == 3);
    const SelftestRecord *stored = selftestResults();
    assert(stored == (const SelftestRecord *)flash && stored->intervals == result->intervals);
    flash[5] ^= 1; // Corruption is detected.
    assert(selftestResults() == nullptr);
    puts("PASS: record storage and validation");
}
