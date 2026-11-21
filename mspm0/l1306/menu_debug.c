#include "ti_msp_dl_config.h"
#include <stdint.h>
#include "audio.h"
#include "board.h"
#include <math.h>

// Main-loop producer and consumer only. Drop an entire line if the queue fills;
// never block audio sampling waiting for serial output.
static char queue[256];
static unsigned head, tail;
volatile uint32_t menuDebugDroppedLines;

static bool measuring;
static const char captureCommands[] = "ORGHA";
static char captureCommand;
static AudioCaptureMode captureMode;
static unsigned quietWindows;
static bool captureStarted;
static const uint16_t *captureSamples;
static unsigned captureTxOffset;

static void cancelCapture(void)
{
    // Once a reply starts, finish the frame even if M or X arrives.
    if (captureSamples) return;
    audioCaptureCancel();
    captureCommand = 0;
    captureStarted = false;
}

static void transmitCapture(void)
{
    static const char header[] = "D,?,512\r\n";
    static const char hex[] = "0123456789ABCDEF";
    const unsigned headerLength = sizeof(header) - 1;
    const unsigned payloadEnd = headerLength + 3 * AUDIO_CAPTURE_SAMPLES;

    while (!DL_UART_Main_isTXFIFOFull(DEBUG_UART_INST)) {
        char byte;
        if (captureTxOffset < headerLength) {
            byte = captureTxOffset == 2 ? captureCommand : header[captureTxOffset];
        } else if (captureTxOffset < payloadEnd) {
            const unsigned digit = captureTxOffset - headerLength;
            byte = hex[(captureSamples[digit / 3] >> (4 * (2 - digit % 3))) & 15];
        } else {
            byte = captureTxOffset == payloadEnd ? '\r' : '\n';
        }
        DL_UART_Main_transmitData(DEBUG_UART_INST, byte);
        if (++captureTxOffset == payloadEnd + 2) {
            captureSamples = 0;
            cancelCapture();
            return;
        }
    }
}
static unsigned windows;
static unsigned settlingWindows;
static uint32_t sequence;
static float gpampPower, gpampMean, gpampPeak;
static uint16_t gpampMin=4095, gpampMax;
static float opaPower, rawPower, opaMean, rawMean, opaPeak, rawPeak;
static uint16_t opaMin=4095, rawMin=4095, opaMax, rawMax;
extern volatile uint32_t micMissedBlocks, micAdcOverruns, micTriggerOverruns;

static void resetWindow(void)
{
    windows=0;
    gpampPower=gpampMean=gpampPeak=0;
    gpampMin=4095; gpampMax=0;
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
    if (captureCommand) return;
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
        if (command == 'M' || command == 'X') {
            cancelCapture();
            measuring = command == 'M';
            boardGpampEnable(measuring);
            if (measuring) {
                settlingWindows = 64;
                sequence = 0;
            }
            resetWindow();
        } else if (command == 'L' || command == 'l') {
            boardLedPwmEnable(command == 'l');
        } else if (measuring && !captureCommand) {
            for (unsigned i = 0; i < sizeof(captureCommands) - 1; ++i) {
                if (command != captureCommands[i]) continue;
                captureCommand = command;
                captureMode = (AudioCaptureMode)i;
                // The first window boundary may be immediately after TX idles.
                quietWindows = 3;
                resetWindow();
                break;
            }
        }
    }
    while (tail != head && !DL_UART_Main_isTXFIFOFull(DEBUG_UART_INST)) {
        DL_UART_Main_transmitData(DEBUG_UART_INST, queue[tail]);
        tail = (tail + 1) & 255;
    }
    if (captureStarted && !captureSamples) {
        captureSamples = audioCaptureResult();
        captureTxOffset = 0;
    }
    if (captureSamples) transmitCapture();
}

// Integer-only CSV formatting avoids printf and large floating-point libraries.
// Static scratch space avoids putting the CSV line on the MCU stack.
static char measurementLine[240];
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
    if (settlingWindows) { --settlingWindows; return; }
    if (captureCommand) {
        if (!captureStarted && tail == head &&
            !DL_UART_Main_isBusy(DEBUG_UART_INST) && --quietWindows == 0) {
            audioCaptureStart(captureMode);
            captureStarted = true;
        }
        return;
    }
    gpampPower+=w->gpamp.rms*w->gpamp.rms;
    gpampMean+=w->gpamp.mean;
    if (w->gpamp.rms>gpampPeak) gpampPeak=w->gpamp.rms;
    if (w->gpamp.minimum<gpampMin) gpampMin=w->gpamp.minimum;
    if (w->gpamp.maximum>gpampMax) gpampMax=w->gpamp.maximum;
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
    measurementLine[n++]='N'; measurementLine[n++]='2';
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
    n=appendNumber(n,(uint32_t)(gpampMean*62.5f+0.5f));
    n=appendNumber(n,(uint32_t)(sqrtf(gpampPower/16)*1000+0.5f));
    n=appendNumber(n,(uint32_t)(gpampPeak*1000+0.5f));
    n=appendNumber(n,gpampMin); n=appendNumber(n,gpampMax);
    n=appendNumber(n,micMissedBlocks);
    n=appendNumber(n,micAdcOverruns);
    n=appendNumber(n,micTriggerOverruns);
    n=appendNumber(n,menuDebugDroppedLines);
    measurementLine[n++]='\r'; measurementLine[n++]='\n';
    sendLine(measurementLine,n);
    resetWindow();
}
