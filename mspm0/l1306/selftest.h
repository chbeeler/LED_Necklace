#ifndef AEQ_L1306_SELFTEST_H
#define AEQ_L1306_SELFTEST_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// False-flash self-test. All amplifier paths are evaluated on the same sound,
// without a PC: for every 1.024 s interval and every path it finds the lowest
// input gain (the UART GAIN setting, at SENS 100 and FLOOR 0) at which the AEQ
// algorithm would have flashed, and counts intervals per gain step. Results
// are kept in flash and printed on request.
//
// Paths: 0 OPA slot 0, 1 OPA slot 1, 2 OPA average of both,
//        3 GPAMP slot 2, 4 GPAMP slot 3, 5 GPAMP average of both.
enum { SELFTEST_PATHS = 6, SELFTEST_BINS = 24 };
enum { SELFTEST_INTERVAL_WINDOWS = 64, SELFTEST_SETTLE_WINDOWS = 1860 };

typedef struct {
    uint32_t magic;
    uint32_t intervals; // Counted 1.024 s intervals.
    // flashIntervals[path][bin]: intervals that flash at gain selftestBinGain(bin).
    uint16_t flashIntervals[SELFTEST_PATHS][SELFTEST_BINS];
    uint16_t meanRmsMilli[SELFTEST_PATHS]; // Mean window RMS, 1/1000 ADC count.
    uint32_t checksum;
} SelftestRecord;

// The AEQ algorithm's level tracking is linear in its input, so one tracker
// at gain 1 answers the question for every gain.
typedef struct {
    float envelope, baseline, peak;
} SelftestTracker;

// Feeds one window; unitSignal is the algorithm's input at GAIN 1. Returns the
// beat (0..1) the algorithm produces once its gates are open, and stores the
// gain above which they are open (INFINITY if no gain opens them).
float selftestTrack(SelftestTracker *tracker, float unitSignal, float *gainNeeded);
// The algorithm's input at GAIN 1 for a window RMS in ADC counts.
float selftestUnitSignal(float rmsCounts);
// Gain of a histogram step: 2, 2.38, 2.83, ... (four steps per octave).
float selftestBinGain(unsigned bin);
// First step whose gain exceeds gainNeeded; SELFTEST_BINS if none does.
unsigned selftestBin(float gainNeeded);

void selftestStart(void);
void selftestStop(void);
bool selftestActive(void);
void selftestWindow(const float rmsCounts[SELFTEST_PATHS]);
uint16_t selftestLedDuty(void);
// The running test's counts, else the stored record, else NULL.
const SelftestRecord *selftestResults(void);

#ifdef __cplusplus
}
#endif
#endif
