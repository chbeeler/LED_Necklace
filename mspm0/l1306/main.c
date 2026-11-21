#include "board.h"
#include "audio.h"
#include "audio_range.h"
#include "ti_msp_dl_config.h"

void menuDebugPoll(void);
void menuDebugMeasure(const AudioWindow *window);
bool updateButtons(bool plus, bool minus, uint32_t nowMs);
float updateLedBrightness(float adcRms, uint16_t windowLength);
extern volatile float micNoiseFloor;
extern volatile float micInputGain;

volatile bool buttonPlus;
volatile bool buttonMinus;
volatile AudioLevel micOpa;
volatile AudioLevel micRaw;
volatile AudioLevel micGpamp;
volatile float micOpaGain = 16.0f;
volatile float micInputRms;
volatile bool micUsingRaw;
volatile uint32_t micRangeChanges;
volatile uint32_t micRawClippedWindows;
volatile float micOpaRmsPeak;
volatile float micRawRmsPeak;
volatile float ledBrightness;
volatile float ledBrightnessPeak;
volatile uint16_t ledDuty;
// Make the onboard LED visible while preserving the shared necklace settings.
volatile float ledDisplayGain = 16.0f;

int main(void)
{
    AudioRange range = {0};
    boardInit();
    // Input-equivalent counts at the C1104's 3.3 V reference, for the shared
    // application's gain/floor controls. These are initial SPW2430 settings.
    micNoiseFloor = 0.25f;
    micInputGain = 12.0f;
    audioStart();
    boardLedPwmEnable(true);
    for (;;) {
        menuDebugPoll();
        AudioWindow window;
        if (!audioPollWindow(&window)) {
            __WFE();
            continue;
        }
        menuDebugMeasure(&window);
        micOpa = window.opa;
        micRaw = window.raw;
        micGpamp = window.gpamp;
        if (window.opa.rms > micOpaRmsPeak) micOpaRmsPeak = window.opa.rms;
        if (window.raw.rms > micRawRmsPeak) micRawRmsPeak = window.raw.rms;
        const bool wasRaw = range.useRaw;
        const float inputRms = audioSelectRms(&range, &window, micOpaGain);
        micUsingRaw = range.useRaw;
        if (wasRaw != range.useRaw) ++micRangeChanges;
        if (window.raw.minimum < 16 || window.raw.maximum > 4079)
            ++micRawClippedWindows;
        micInputRms = inputRms * (2.5f / 3.3f);
        buttonPlus = boardPlusPressed();
        buttonMinus = boardMinusPressed();
        // Power-off/standby is deferred; the two-button off request is ignored.
        (void)updateButtons(buttonPlus, buttonMinus, window.timeMs);
        ledBrightness = updateLedBrightness(micInputRms, AUDIO_WINDOW_PAIRS);
        if (ledBrightness > ledBrightnessPeak) ledBrightnessPeak = ledBrightness;
        float display = ledBrightness * ledDisplayGain;
        if (!(display >= 0.0f)) display = 0.0f;
        if (display > 1.0f) display = 1.0f;
        ledDuty = (uint16_t)(display * 4095.0f + 0.5f);
        // Applied at the end of the window now being sampled (16 ms later).
        boardSetLed(ledDuty);
    }
}
