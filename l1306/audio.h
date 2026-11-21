#ifndef AEQ_L1306_AUDIO_H
#define AEQ_L1306_AUDIO_H
#include <stdbool.h>
#include <stdint.h>
enum { AUDIO_SAMPLE_RATE = 16000, AUDIO_WINDOW_PAIRS = 256 };
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
    uint32_t timeMs;
} AudioWindow;
void audioStart(void);
bool audioPollWindow(AudioWindow *window);
#endif
