#include "audio_range.h"

float audioSelectRms(AudioRange *range, const AudioWindow *window, float opaGain)
{
    // Conservative initial margins for a 2.5 V ADC range. Validate them with
    // the assembled analog circuit; software cannot undo amplifier clipping.
    const bool clipped = window->opa.minimum < 256 || window->opa.maximum > 3967;
    const bool clear = window->opa.minimum >= 384 && window->opa.maximum <= 3839;
    if (clipped) {
        range->useRaw = true;
        range->clearWindows = 0;
    } else if (range->useRaw) {
        range->clearWindows = clear ? range->clearWindows + 1 : 0;
        if (range->clearWindows >= 8) {
            range->useRaw = false;
            range->clearWindows = 0;
        }
    }
    // A CCS gain override calibrates the actual OPA/raw amplitude ratio.
    // Reject invalid debugger values, including NaN.
    if (!(opaGain >= 1.0f && opaGain <= 32.0f)) opaGain = 16.0f;
    return range->useRaw ? window->raw.rms : window->opa.rms / opaGain;
}
