#ifndef AEQ_L1306_AUDIO_RANGE_H
#define AEQ_L1306_AUDIO_RANGE_H
#include "audio.h"
typedef struct {
    bool useRaw;
    uint8_t clearWindows;
} AudioRange;
float audioSelectRms(AudioRange *range, const AudioWindow *window, float opaGain);
#endif
