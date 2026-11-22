#ifndef AEQ_L1306_BOARD_H
#define AEQ_L1306_BOARD_H
#include <stdbool.h>
#include <stdint.h>
void boardInit(void);
bool boardPlusPressed(void);
bool boardMinusPressed(void);
// Stores the duty (0..4095); it takes effect at the next boardLatchLed().
void boardSetLed(uint16_t duty);
void boardLatchLed(void);
void boardGpampEnable(bool enable);
void boardSelectAmplifier(bool gpamp);
void boardEnterBootloader(void);
void boardLedPwmEnable(bool enable);
#endif
