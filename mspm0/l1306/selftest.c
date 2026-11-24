#include "selftest.h"
#include "audio.h"
#include "board.h"
#include <math.h>

_Static_assert(sizeof(SelftestRecord) == 312 && sizeof(SelftestRecord) % 8 == 0,
               "the record is stored as 64-bit flash words");
_Static_assert((int)SELFTEST_PATHS == (int)AUDIO_TEST_LEVELS,
               "one path per test level");

enum { RECORD_MAGIC = 0x31514541, RECORD_WORDS = sizeof(SelftestRecord) / 4 };
enum { SAVE_INTERVALS = 128, SKIP_AFTER_SAVE = 4 };

// These mirror AdaptiveBrightness::processRms and updateLedBrightness() for
// SENS 100, FLOOR 0, the default 60 ms afterglow and the 16 ms window;
// tests/selftest_test.cpp checks them against that code.
#define WINDOW_SECONDS ((float)AUDIO_WINDOW_PAIRS / AUDIO_SAMPLE_RATE)
#define NOISE_GATE (10.0f / 32768.0f)
#define MODULATION_GATE (2.0f * NOISE_GATE * 0.25f) // 0.25 = sensitivity 100
// LED duty rises visibly above its idle level from about this beat.
#define FLASH_BEAT 0.12f

static float attack, release, baselineUp, baselineDown, peakDecay;

static void initCoefficients(void)
{
    attack = 1.0f - expf(-WINDOW_SECONDS / 0.020f);
    release = 1.0f - expf(-WINDOW_SECONDS / 0.060f);
    baselineUp = 1.0f - expf(-WINDOW_SECONDS / 1.20f);
    baselineDown = 1.0f - expf(-WINDOW_SECONDS / 2.00f);
    peakDecay = expf(-WINDOW_SECONDS / 2.0f);
}

float selftestUnitSignal(float rmsCounts)
{
    // main.c: amplifier RMS / 16, rescaled from the 2.5 V to a 3.3 V range;
    // updateLedBrightness(): times GAIN, as a fraction of 2048 counts.
    return rmsCounts * (2.5f / 3.3f / 16.0f / 2048.0f);
}

float selftestTrack(SelftestTracker *t, float signal, float *gainNeeded)
{
    if (attack == 0.0f) initCoefficients();
    if (!(signal >= 0.0f)) signal = 0.0f;
    t->envelope += (signal > t->envelope ? attack : release)
                 * (signal - t->envelope);
    t->baseline += (t->envelope > t->baseline ? baselineUp : baselineDown)
                 * (t->envelope - t->baseline);
    float modulation = t->envelope - t->baseline;
    if (modulation < 0.0f) modulation = 0.0f;
    const float decayed = t->peak * peakDecay;
    t->peak = modulation > decayed ? modulation : decayed;

    *gainNeeded = INFINITY;
    if (!(t->envelope > 0.0f && t->peak > 0.0f)) return 0.0f;
    // At gain g the algorithm sees g * envelope and g * peak.
    const float forNoise = NOISE_GATE / t->envelope;
    const float forModulation = MODULATION_GATE / t->peak;
    *gainNeeded = forNoise > forModulation ? forNoise : forModulation;
    return modulation / t->peak;
}

float selftestBinGain(unsigned bin)
{
    float gain = 2.0f;
    while (bin--) gain *= 1.189207115f; // 2^(1/4)
    return gain;
}

unsigned selftestBin(float gainNeeded)
{
    float gain = 2.0f;
    for (unsigned bin = 0; bin < SELFTEST_BINS; ++bin, gain *= 1.189207115f)
        if (gainNeeded < gain) return bin;
    return SELFTEST_BINS;
}

static bool active;
static SelftestRecord record;
static SelftestTracker trackers[SELFTEST_PATHS];
static float lowestGain[SELFTEST_PATHS]; // Within the current interval.
static float rmsSum[SELFTEST_PATHS], rmsMean[SELFTEST_PATHS];
static unsigned settleWindows, skipWindows, intervalWindows;

static void beginInterval(void)
{
    intervalWindows = 0;
    for (unsigned p = 0; p < SELFTEST_PATHS; ++p) {
        lowestGain[p] = INFINITY;
        rmsSum[p] = 0.0f;
    }
}

static uint32_t checksum(const SelftestRecord *r)
{
    const uint32_t *words = (const uint32_t *)r;
    uint32_t sum = 0;
    for (unsigned i = 0; i < RECORD_WORDS - 1; ++i) sum += words[i];
    return sum;
}

static void save(void)
{
    record.checksum = checksum(&record);
    boardStoreRecord((const uint32_t *)&record, RECORD_WORDS);
    // The flash erase pauses sampling and may disturb the analog supply.
    skipWindows = SKIP_AFTER_SAVE;
    beginInterval();
}

void selftestStart(void)
{
    record = (SelftestRecord){.magic = RECORD_MAGIC};
    for (unsigned p = 0; p < SELFTEST_PATHS; ++p) {
        trackers[p] = (SelftestTracker){0};
        rmsMean[p] = 0.0f;
    }
    settleWindows = SELFTEST_SETTLE_WINDOWS;
    skipWindows = 0;
    beginInterval();
    boardGpampEnable(true);
    boardSelectTestLayout();
    audioSetTestMode(true);
    active = true;
}

void selftestStop(void)
{
    if (!active) return;
    save();
    audioSetTestMode(false);
    active = false;
}

bool selftestActive(void) { return active; }

// Bright for the first two seconds as a start signal, then a steady dim level
// so the LED keeps switching as in use but cannot feed back into the result.
uint16_t selftestLedDuty(void)
{
    return settleWindows > SELFTEST_SETTLE_WINDOWS - 125 ? 800 : 64;
}

void selftestWindow(const float rmsCounts[SELFTEST_PATHS])
{
    if (!active) return;
    if (skipWindows) { --skipWindows; return; }
    for (unsigned p = 0; p < SELFTEST_PATHS; ++p) {
        float gainNeeded;
        const float beat = selftestTrack(&trackers[p],
                               selftestUnitSignal(rmsCounts[p]), &gainNeeded);
        if (beat >= FLASH_BEAT && gainNeeded < lowestGain[p])
            lowestGain[p] = gainNeeded;
        rmsSum[p] += rmsCounts[p];
    }
    // The room empties and the amplifiers settle before counting starts.
    if (settleWindows) {
        if (--settleWindows == 0) beginInterval();
        return;
    }
    if (++intervalWindows < SELFTEST_INTERVAL_WINDOWS) return;

    ++record.intervals;
    for (unsigned p = 0; p < SELFTEST_PATHS; ++p) {
        for (unsigned bin = selftestBin(lowestGain[p]); bin < SELFTEST_BINS; ++bin)
            if (record.flashIntervals[p][bin] != UINT16_MAX)
                ++record.flashIntervals[p][bin];
        rmsMean[p] += (rmsSum[p] / SELFTEST_INTERVAL_WINDOWS - rmsMean[p])
                    / (float)record.intervals;
        const float milli = rmsMean[p] * 1000.0f + 0.5f;
        record.meanRmsMilli[p] = milli < 65535.0f ? (uint16_t)milli : UINT16_MAX;
    }
    beginInterval();
    if (record.intervals % SAVE_INTERVALS == 0) save();
}

const SelftestRecord *selftestResults(void)
{
    if (active) return &record;
    const SelftestRecord *stored = (const SelftestRecord *)boardStoredRecord();
    if (stored->magic != RECORD_MAGIC || stored->checksum != checksum(stored))
        return 0;
    return stored;
}
