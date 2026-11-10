#include "adaptive_brightness.h"
#include "button_gestures.h"
#include "button_settings.h"
#include "led_modes.h"

namespace {
AdaptiveBrightness liveBrightness;
ButtonGestures buttons;
uint16_t brightnessLimit = ButtonSettings::DEFAULT_BRIGHTNESS;
uint8_t idlePercent = 4;
LedMode mode = MODE_AEQ;
}

// Quiet-room cutoff in 12-bit ADC counts RMS; both settings are editable in CCS.
volatile float micNoiseFloor = 1.2f;
volatile float micInputGain = 12.0f;

extern "C" void updateButtons(bool plus, bool minus, uint32_t nowMs)
{
    const ButtonAction action = buttons.update(plus, minus, nowMs);
    switch (action) {
    case ButtonAction::BrightnessUp:
    case ButtonAction::BrightnessDown:
        brightnessLimit = ButtonSettings::stepBrightness(brightnessLimit, action == ButtonAction::BrightnessUp);
        break;
    case ButtonAction::SecondaryUp:
    case ButtonAction::SecondaryDown:
        if (mode == MODE_AEQ)
            idlePercent = ButtonSettings::stepIdlePercent(idlePercent, action == ButtonAction::SecondaryUp);
        else
            brightnessLimit = ButtonSettings::fineBrightness(brightnessLimit, action == ButtonAction::SecondaryUp);
        break;
    case ButtonAction::ToggleAeqConstant:
        mode = mode == MODE_AEQ ? MODE_CONSTANT : MODE_AEQ;
        if (mode == MODE_AEQ) liveBrightness.reset();
        break;
    case ButtonAction::RequestSystemOff:
    case ButtonAction::None:
        break;
    }
}

extern "C" float updateLedBrightness(float adcRms, uint16_t windowSamples)
{
    const float limit = brightnessLimit / 4095.0f;
    if (mode == MODE_CONSTANT) return limit;

    // A centered 12-bit sample spans +/-2048 counts, or +/-32768 in PCM16.
    const float aboveNoise = adcRms - micNoiseFloor;
    const float signal = aboveNoise > 0.0f ? aboveNoise * micInputGain : 0.0f;
    return limit * liveBrightness.processRms(signal / 2048.0f, windowSamples / 16000.0f,
                                     10.0f, 0.060f, idlePercent / 100.0f);
}
