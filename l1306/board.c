#include "board.h"
#include "ti_msp_dl_config.h"

void boardInit(void)
{
    SYSCFG_DL_init();
    boardSetLed(0);
    DL_TimerG_startCounter(LED_PWM_INST);
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
    // SysConfig maps 0% to PERIOD and 100% to zero for this PWM mode.
    const uint32_t compare = duty == 4095 ? 0 : 4096 - duty;
    DL_TimerG_setCaptureCompareValue(LED_PWM_INST, compare, DL_TIMER_CC_1_INDEX);
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
