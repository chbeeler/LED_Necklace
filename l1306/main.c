#include "board.h"
#include "ti_msp_dl_config.h"
volatile bool buttonPlus;
volatile bool buttonMinus;
int main(void)
{
    boardInit();
    for (;;) {
        buttonPlus = boardPlusPressed();
        buttonMinus = boardMinusPressed();
        boardSetLed(buttonPlus ? 512 : 0);
        delay_cycles(CPUCLK_FREQ / 1000);
    }
}
