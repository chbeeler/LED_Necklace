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
    return DL_GPIO_readPins(GPIO_BUTTONS_PORT, GPIO_BUTTONS_PLUS_PIN) != 0;
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
