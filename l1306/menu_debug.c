#include "ti_msp_dl_config.h"
#include <stdint.h>

// Main-loop producer and consumer only. Drop an entire line if the queue fills;
// never block audio sampling waiting for serial output.
static char queue[64];
static unsigned head, tail;
volatile uint32_t menuDebugDroppedLines;

void reportAeqMenu(const char *label, uint16_t value)
{
    char line[16], digits[5];
    unsigned count = 0, n = 0;
    while (*label && count < 7) line[count++] = *label++;
    line[count++] = ':';
    line[count++] = ' ';
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    while (n) line[count++] = digits[--n];
    line[count++] = '\r';
    line[count++] = '\n';
    if (count > ((tail - head - 1) & 63)) {
        ++menuDebugDroppedLines;
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        queue[head] = line[i];
        head = (head + 1) & 63;
    }
}

void menuDebugPoll(void)
{
    while (tail != head && !DL_UART_Main_isTXFIFOFull(DEBUG_UART_INST)) {
        DL_UART_Main_transmitData(DEBUG_UART_INST, queue[tail]);
        tail = (tail + 1) & 63;
    }
}
