#ifndef AEQ_L1306_AUDIO_H
#define AEQ_L1306_AUDIO_H
#include <stdbool.h>
#include <stdint.h>
enum { AUDIO_SAMPLE_RATE = 16000, AUDIO_WINDOW_PAIRS = 256 };
enum { AUDIO_TEST_LEVELS = 6 };
typedef struct {
    float mean;
    float rms;
    uint16_t minimum;
    uint16_t maximum;
} AudioLevel;
typedef struct {
    AudioLevel opa;
    AudioLevel raw;
    AudioLevel gpamp;
    // The amplified signal the LED algorithm uses: ADC slot 0 alone, or the
    // average of slots 0, 2 and 3 (see audioSetAveraging). In ADC counts.
    AudioLevel amp;
    // Self-test only (audioSetTestMode): AC RMS of slot 0, slot 1, their
    // average, slot 2, slot 3, and their average.
    float testRms[AUDIO_TEST_LEVELS];
    uint32_t timeMs;
} AudioWindow;
void audioSetTestMode(bool enable);
// Slots 0, 2 and 3 must all convert the same amplifier while averaging is on.
// Restarts the current window, so the next reported one is consistent.
void audioSetAveraging(bool averageThree);
enum { AUDIO_CAPTURE_SAMPLES = 512 };
typedef enum {
    AUDIO_CAPTURE_OPA,
    AUDIO_CAPTURE_RAW,
    AUDIO_CAPTURE_GPAMP,
    AUDIO_CAPTURE_GPAMP_REPEAT,
    AUDIO_CAPTURE_ALL,
} AudioCaptureMode;
void audioCaptureStart(AudioCaptureMode mode);
void audioCaptureCancel(void);
const uint16_t *audioCaptureResult(void);
void audioStart(void);
bool audioPollWindow(AudioWindow *window);
#endif
