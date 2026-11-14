#include "adaptive_brightness.h"
#include "aeq_button_menu.h"
#include "button_gestures.h"
#include "button_settings.h"
#include "led_modes.h"

namespace {
AdaptiveBrightness liveBrightness;
ButtonGestures buttons;
AeqButtonMenu menu;
uint16_t brightnessLimit = ButtonSettings::DEFAULT_BRIGHTNESS;
uint8_t idlePercent = 4;
uint8_t aeqSensitivity = 50;
uint16_t aeqAfterglowMs = 60;
LedMode mode = MODE_AEQ;
uint32_t lastNowMs = 0;

void adjustMenu(bool increase, bool fine)
{
    menu.touch(lastNowMs);
    switch (menu.item()) {
    case AeqMenuItem::Sensitivity:
        aeqSensitivity = ButtonSettings::stepAeqSensitivity(aeqSensitivity, increase, fine);
        break;
    case AeqMenuItem::Afterglow:
        aeqAfterglowMs = ButtonSettings::stepAeqReleaseMs(aeqAfterglowMs, increase, fine);
        break;
    case AeqMenuItem::IdleBrightness:
        idlePercent = ButtonSettings::stepIdlePercent(idlePercent, increase);
        break;
    case AeqMenuItem::None:
        break;
    }
}
}

// Quiet-room cutoff in 12-bit ADC counts RMS; both settings are editable in CCS.
volatile float micNoiseFloor = 1.2f;
volatile float micInputGain = 12.0f;

extern "C" bool updateButtons(bool plus, bool minus, uint32_t nowMs)
{
    lastNowMs = nowMs;
    if (mode != MODE_AEQ && menu.active()) menu.cancel();
    menu.update(nowMs);
    const ButtonAction action = buttons.update(plus, minus, nowMs);
    switch (action) {
    case ButtonAction::BrightnessUp:
    case ButtonAction::BrightnessDown:
        if (menu.active())
            adjustMenu(action == ButtonAction::BrightnessUp, false);
        else
            brightnessLimit = ButtonSettings::stepBrightness(brightnessLimit, action == ButtonAction::BrightnessUp);
        break;
    case ButtonAction::SecondaryUp:
    case ButtonAction::SecondaryDown:
        if (menu.active())
            adjustMenu(action == ButtonAction::SecondaryUp, true);
        else if (mode == MODE_AEQ)
            idlePercent = ButtonSettings::stepIdlePercent(idlePercent, action == ButtonAction::SecondaryUp);
        else
            brightnessLimit = ButtonSettings::fineBrightness(brightnessLimit, action == ButtonAction::SecondaryUp);
        break;
    case ButtonAction::AdvanceAeqMenu:
        if (mode == MODE_AEQ) menu.advance(nowMs);
        break;
    case ButtonAction::ToggleAeqConstant:
        if (menu.active())
            menu.exit(nowMs);
        else {
            mode = mode == MODE_AEQ ? MODE_CONSTANT : MODE_AEQ;
            if (mode == MODE_AEQ) liveBrightness.reset();
        }
        break;
    case ButtonAction::RequestSystemOff:
        menu.cancel();
        return true;
    case ButtonAction::None:
        break;
    }
    return false;
}

extern "C" float updateLedBrightness(float adcRms, uint16_t windowSamples)
{
    const float limit = brightnessLimit / 4095.0f;
    if (mode == MODE_CONSTANT) return limit;

    float feedback = 0.0f;
    if (menu.feedbackLevel(lastNowMs, feedback)) {
        const float cueLimit = limit > (100.0f / 4095.0f)
                                   ? limit
                                   : (100.0f / 4095.0f);
        return feedback * cueLimit;
    }
    if (menu.forcesIdleOutput()) return limit * idlePercent / 100.0f;

    // A centered 12-bit sample spans +/-2048 counts, or +/-32768 in PCM16.
    const float aboveNoise = adcRms - micNoiseFloor;
    const float signal = aboveNoise > 0.0f ? aboveNoise * micInputGain : 0.0f;
    return limit * liveBrightness.processRms(
                       signal / 2048.0f, windowSamples / 16000.0f, 10.0f,
                       aeqSensitivity, aeqAfterglowMs / 1000.0f,
                       idlePercent / 100.0f);
}
