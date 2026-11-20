#include "ti_msp_dl_config.h"
#include <stdint.h>
#include "audio.h"
#include <math.h>

// Main-loop producer and consumer only. Drop an entire line if the queue fills;
// never block audio sampling waiting for serial output.
static char queue[256];
static unsigned head, tail;
volatile uint32_t menuDebugDroppedLines;

static bool measuring;
static unsigned windows;
static uint32_t sequence;
static float opaPower, rawPower, opaMean, rawMean, opaPeak, rawPeak;
static uint16_t opaMin=4095, rawMin=4095, opaMax, rawMax;
extern volatile uint32_t micMissedBlocks, micAdcOverruns, micTriggerOverruns;

static void resetWindow(void)
{
    windows=0;
    opaPower=rawPower=opaMean=rawMean=opaPeak=rawPeak=0;
    opaMin=rawMin=4095;
    opaMax=rawMax=0;
}

static void sendLine(const char *line, unsigned count)
{
    if (count > ((tail-head-1)&255)) { ++menuDebugDroppedLines; return; }
    for (unsigned i=0; i<count; ++i) {
        queue[head]=line[i]; head=(head+1)&255;
    }
}

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
    sendLine(line, count);
}

void menuDebugPoll(void)
{
    while (!DL_UART_Main_isRXFIFOEmpty(DEBUG_UART_INST)) {
        const uint8_t command=DL_UART_Main_receiveData(DEBUG_UART_INST);
        if (command=='M') { measuring=true; sequence=0; resetWindow(); }
        if (command=='X') { measuring=false; resetWindow(); }
    }
    while (tail != head && !DL_UART_Main_isTXFIFOFull(DEBUG_UART_INST)) {
        DL_UART_Main_transmitData(DEBUG_UART_INST, queue[tail]);
        tail = (tail + 1) & 255;
    }
}

// Integer-only CSV formatting avoids printf and large floating-point libraries.
// Static scratch space avoids putting the CSV line on the MCU stack.
static char measurementLine[192];
static unsigned appendNumber(unsigned n, uint32_t value)
{
    char digits[10]; unsigned count=0;
    do { digits[count++]='0'+value%10; value/=10; } while(value);
    measurementLine[n++]=',';
    while(count) measurementLine[n++]=digits[--count];
    return n;
}

void menuDebugMeasure(const AudioWindow *w)
{
    if (!measuring) return;
    opaPower+=w->opa.rms*w->opa.rms; rawPower+=w->raw.rms*w->raw.rms;
    opaMean+=w->opa.mean; rawMean+=w->raw.mean;
    if (w->opa.rms>opaPeak) opaPeak=w->opa.rms;
    if (w->raw.rms>rawPeak) rawPeak=w->raw.rms;
    if (w->opa.minimum<opaMin) opaMin=w->opa.minimum;
    if (w->raw.minimum<rawMin) rawMin=w->raw.minimum;
    if (w->opa.maximum>opaMax) opaMax=w->opa.maximum;
    if (w->raw.maximum>rawMax) rawMax=w->raw.maximum;
    if (++windows<16) return;
    unsigned n=0;
    measurementLine[n++]='N'; measurementLine[n++]='1';
    n=appendNumber(n,++sequence);
    n=appendNumber(n,w->timeMs);
    n=appendNumber(n,(uint32_t)(opaMean*62.5f+0.5f));
    n=appendNumber(n,(uint32_t)(sqrtf(opaPower/16)*1000+0.5f));
    n=appendNumber(n,(uint32_t)(opaPeak*1000+0.5f));
    n=appendNumber(n,opaMin); n=appendNumber(n,opaMax);
    n=appendNumber(n,(uint32_t)(rawMean*62.5f+0.5f));
    n=appendNumber(n,(uint32_t)(sqrtf(rawPower/16)*1000+0.5f));
    n=appendNumber(n,(uint32_t)(rawPeak*1000+0.5f));
    n=appendNumber(n,rawMin); n=appendNumber(n,rawMax);
    n=appendNumber(n,micMissedBlocks);
    n=appendNumber(n,micAdcOverruns);
    n=appendNumber(n,micTriggerOverruns);
    n=appendNumber(n,menuDebugDroppedLines);
    measurementLine[n++]='\r'; measurementLine[n++]='\n';
    sendLine(measurementLine,n);
    resetWindow();
}
