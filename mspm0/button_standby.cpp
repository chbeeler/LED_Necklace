#include "button_controller.h"
#include "ti_msp_dl_config.h"

namespace {
constexpr uint32_t buttonMask = GPIO_BUTTONS_PLUS_PIN | GPIO_BUTTONS_MINUS_PIN;
constexpr uint32_t pollMs = 5;

// Standby wakes on GPIO edges. Keep PRIMASK set from entry until reset so a
// pending edge wakes WFI even if it occurs between the input read and WFI.
void waitForButtons(uint32_t pressed, uint32_t stableMs)
{
    uint32_t elapsed = 0;
    for (;;) {
        DL_GPIO_clearInterruptStatus(GPIO_BUTTONS_PORT, buttonMask);
        NVIC_ClearPendingIRQ(GPIOA_INT_IRQn);
        const uint32_t current = buttonMask & ~DL_GPIO_readPins(GPIO_BUTTONS_PORT, buttonMask);
        if (current != pressed) {
            elapsed = 0;
            __DSB();
            __WFI();
            continue;
        }

        delay_cycles(CPUCLK_FREQ / 1000 * pollMs);
        // An intervening release/repress also breaks the continuous hold.
        if (DL_GPIO_getRawInterruptStatus(GPIO_BUTTONS_PORT, buttonMask)) {
            elapsed = 0;
        } else {
            elapsed += pollMs;
            if (elapsed >= stableMs) return;
        }
    }
}
}

extern "C" void enterButtonStandby(void)
{
    __disable_irq();
    DL_TimerG_stopCounter(TIMER_0_INST);
    DL_ADC12_disableConversions(ADC12_0_INST);
    NVIC_DisableIRQ(ADC12_0_INST_INT_IRQN);
    DL_ADC12_disablePower(ADC12_0_INST);
    DL_TimerG_disablePower(TIMER_0_INST);
    DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_PIN);

    DL_GPIO_setUpperPinsPolarity(GPIO_BUTTONS_PORT,
        DL_GPIO_PIN_23_EDGE_RISE_FALL | DL_GPIO_PIN_24_EDGE_RISE_FALL);
    DL_GPIO_enableInterrupt(GPIO_BUTTONS_PORT, buttonMask);
    NVIC_EnableIRQ(GPIOA_INT_IRQn);
    DL_SYSCTL_disableSleepOnExit();
    DL_SYSCTL_setPowerPolicySTANDBY0();

    waitForButtons(0, ButtonController::RELEASE_GUARD_MS);
    waitForButtons(buttonMask, ButtonGestures::LONG_PRESS_MS);
    waitForButtons(0, ButtonController::RELEASE_GUARD_MS);
    NVIC_SystemReset();
}
