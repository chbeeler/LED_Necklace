#include "audio_range.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    AudioRange range = {0};
    AudioWindow frame = {
        .opa = {.rms = 160, .minimum = 1000, .maximum = 2000},
        .raw = {.rms = 10, .minimum = 1300, .maximum = 1500},
    };
    assert(audioSelectRms(&range, &frame, 16) == 10);
    assert(!range.useRaw);
    frame.opa.minimum = 255;
    assert(audioSelectRms(&range, &frame, 16) == 10 && range.useRaw);
    frame.opa.minimum = 1000;
    for (int i = 0; i < 7; ++i) {
        audioSelectRms(&range, &frame, 16);
        assert(range.useRaw);
    }
    // A marginal window restarts the recovery interval.
    frame.opa.minimum = 383;
    audioSelectRms(&range, &frame, 16);
    assert(range.clearWindows == 0 && range.useRaw);
    frame.opa.minimum = 384;
    for (int i = 0; i < 8; ++i) audioSelectRms(&range, &frame, 16);
    assert(!range.useRaw);
    assert(audioSelectRms(&range, &frame, 8) == 20);
    assert(audioSelectRms(&range, &frame, 0) == 10);
    assert(audioSelectRms(&range, &frame, NAN) == 10);
    frame.opa.maximum = 3968;
    frame.raw.rms = 20;
    assert(audioSelectRms(&range, &frame, 16) == 20 && range.useRaw);
    puts("Range normalization, both clipping rails, hysteresis and invalid gain passed.");
    return 0;
}
