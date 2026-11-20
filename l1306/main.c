#include "board.h"
#include "audio.h"
#include "ti_msp_dl_config.h"

volatile bool buttonPlus;
volatile bool buttonMinus;
volatile AudioLevel micOpa;
volatile AudioLevel micRaw;

int main(void)
{
    boardInit();
    audioStart();
    for (;;) {
        AudioWindow window;
        if (!audioPollWindow(&window)) {
            __WFE();
            continue;
        }
        micOpa = window.opa;
        micRaw = window.raw;
        buttonPlus = boardPlusPressed();
        buttonMinus = boardMinusPressed();
        boardSetLed(buttonPlus ? 512 : 0);
    }
}
