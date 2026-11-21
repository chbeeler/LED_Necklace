#ifndef AEQ_L1306_BOARD_H
#define AEQ_L1306_BOARD_H
#include <stdbool.h>
#include <stdint.h>
void boardInit(void);
bool boardPlusPressed(void);
bool boardMinusPressed(void);
void boardSetLed(uint16_t duty);
void boardGpampEnable(bool enable);
#endif
