// Host-only peripheral stubs for this test.
#include <stdbool.h>
#include <stdint.h>
#define DEBUG_UART_INST 0
bool DL_UART_Main_isRXFIFOEmpty(int);
uint8_t DL_UART_Main_receiveData(int);
bool DL_UART_Main_isTXFIFOFull(int);
void DL_UART_Main_transmitData(int,char);
