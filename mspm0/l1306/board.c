#include "board.h"
#include "ti_msp_dl_config.h"

// LED edges that fall inside an ADC acquisition disturb that sample by up to
// a few hundred counts. The PWM therefore runs at exactly one period per ADC
// frame, so an edge hits the same point of every frame and its disturbance is
// a constant offset instead of noise. Duty changes are applied only at audio
// window boundaries (boardLatchLed), which keeps that offset constant within
// each RMS window, where the mean is removed.
enum {
    LED_PWM_PERIOD = 2000,
    // The LED turns on at each PWM reload. Reload this many timer ticks
    // (17.5 us) before the ADC trigger: the four conversions of a frame take
    // about 40 us, so the turn-on edge lands in the idle gap after them.
    LED_RELOAD_LEAD = 560,
};
_Static_assert(AUDIO_TIMER_INST_LOAD_VALUE + 1 == LED_PWM_PERIOD,
               "LED PWM period must equal the ADC frame period");

static volatile uint32_t pendingCompare = LED_PWM_PERIOD;

void boardInit(void)
{
    SYSCFG_DL_init();
    boardSetLed(0);
    boardLatchLed();
}

// Call with the audio timer running. Both timers count down on BUSCLK, so the
// phase set here is kept until the LED counter is stopped.
static bool ledPwmEnabled;

void boardLedPwmEnable(bool enable)
{
    ledPwmEnabled = enable;
    DL_TimerG_stopCounter(LED_PWM_INST);
    if (!enable) return;
    // The counter may only be written while stopped, and must then keep the
    // written value when it is enabled.
    DL_Timer_setCounterValueAfterEnable(LED_PWM_INST,
                                        DL_TIMER_COUNT_AFTER_EN_NO_CHANGE);
    const uint32_t mask = __get_PRIMASK();
    __disable_irq();
    // Counter values are the ticks left until the ADC trigger / PWM reload.
    const uint32_t untilTrigger = DL_TimerG_getTimerCount(AUDIO_TIMER_INST);
    DL_TimerG_setTimerCount(LED_PWM_INST,
        (untilTrigger + LED_PWM_PERIOD - LED_RELOAD_LEAD) % LED_PWM_PERIOD);
    DL_TimerG_startCounter(LED_PWM_INST);
    __set_PRIMASK(mask);
}

// Points ADC slots 0, 2 and 3 at the OPA (channel 12) or the GPAMP (channel
// 14); slot 1 stays on the unamplified microphone. Sampling pauses for about
// 0.2 ms so the change happens between two frames and the FIFO word order the
// DMA relies on is kept. The caller enables the GPAMP itself.
void boardSelectAmplifier(bool gpamp)
{
    const uint32_t channel = gpamp ? DL_ADC12_INPUT_CHAN_14
                                   : DL_ADC12_INPUT_CHAN_12;
    static const DL_ADC12_MEM_IDX slots[] = {
        DL_ADC12_MEM_IDX_0, DL_ADC12_MEM_IDX_2, DL_ADC12_MEM_IDX_3};
    DL_TimerG_stopCounter(AUDIO_TIMER_INST);
    delay_cycles(CPUCLK_FREQ / 5000); // Let the frame in progress finish.
    DL_ADC12_disableConversions(AUDIO_ADC_INST);
    for (unsigned i = 0; i < 3; ++i) {
        // Same settings as SysConfig generates for these slots, except the
        // channel: the last slot of a frame waits for the next timer event.
        DL_ADC12_configConversionMem(AUDIO_ADC_INST, slots[i], channel,
            DL_ADC12_REFERENCE_VOLTAGE_INTREF,
            DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0,
            DL_ADC12_AVERAGING_MODE_DISABLED,
            DL_ADC12_BURN_OUT_SOURCE_DISABLED,
            slots[i] == DL_ADC12_MEM_IDX_3 ? DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT
                                           : DL_ADC12_TRIGGER_MODE_AUTO_NEXT,
            DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    }
    DL_ADC12_enableConversions(AUDIO_ADC_INST);
    DL_TimerG_startCounter(AUDIO_TIMER_INST);
    // The pause moved the frame timing; put the LED edges back in the gap.
    if (ledPwmEnabled) boardLedPwmEnable(true);
}

bool boardPlusPressed(void)
{
    // PA18 is the GPAMP summing node; S1/J11 must be disconnected.
    return false;
}

bool boardMinusPressed(void)
{
    return DL_GPIO_readPins(GPIO_BUTTONS_PORT, GPIO_BUTTONS_MINUS_PIN) == 0;
}

void boardSetLed(uint16_t duty)
{
    if (duty > 4095) duty = 4095;
    const uint32_t onTicks = ((uint32_t)duty * LED_PWM_PERIOD + 2047) / 4095;
    // SysConfig maps 0% to PERIOD and 100% to zero for this PWM mode.
    pendingCompare = LED_PWM_PERIOD - onTicks;
}

// Called from the DMA interrupt when a block that ends an audio window has
// completed, i.e. in the idle gap after that frame's conversions.
void boardLatchLed(void)
{
    DL_TimerG_setCaptureCompareValue(LED_PWM_INST, pendingCompare,
                                     DL_TIMER_CC_1_INDEX);
}

// Enabled only by the measurement command, never automatically at boot. This
// allows a BSL update on factory RX PA22, followed by moving that wire to PA9.
void boardGpampEnable(bool enable)
{
    if (!enable) { DL_GPAMP_disableOutputToPad(); DL_GPAMP_disable(); return; }
    DL_GPIO_initPeripheralAnalogFunction(IOMUX_PINCM27); // PA26 IN+
    DL_GPIO_initPeripheralAnalogFunction(IOMUX_PINCM19); // PA18 IN-
    DL_GPIO_initPeripheralAnalogFunction(IOMUX_PINCM23); // PA22 OUT
    DL_GPAMP_Config config = {
        .pselChannel = DL_GPAMP_PSEL_IN_POS,
        .nselChannel = DL_GPAMP_NSEL_IN_NEG,
        .outputPinState = DL_GPAMP_OUTPUT_PIN_STATE_ENABLED,
        .choppingMode = DL_GPAMP_CHOPPING_MODE_DISABLED,
        .choppingFreq = DL_GPAMP_CHOPPING_FREQ_2KHZ,
    };
    DL_GPAMP_init(&config);
    DL_GPAMP_setRailToRailInputMode(DL_GPAMP_RRI_MODE_RAIL_TO_RAIL);
    DL_GPAMP_enable();
}
