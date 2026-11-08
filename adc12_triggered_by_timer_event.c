/*
 * Copyright (c) 2020, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ti_msp_dl_config.h"
#include <math.h>

void runAeqFitCheck(void);
float updateLedBrightness(float adcRms, uint16_t windowLength);

enum { windowSamples = 256, adcMax = 4095 };

typedef struct {
    uint32_t sum;
    uint32_t sumSquares;  // 256 * 4095^2 fits in uint32_t.
    uint16_t minimum;
    uint16_t maximum;
} MicWindow;

static MicWindow collecting = {.minimum = adcMax};
static volatile MicWindow completed;
static volatile bool windowReady;
static uint16_t samplesInWindow;
static int16_t adcOffset;

// CCS watch values for the most recently processed 16 ms window, in ADC counts.
volatile float ledBrightness;
static volatile uint16_t ledDuty;  // 0..256
static uint16_t ledAccumulator;
volatile float micMean;
volatile float micRms;  // AC only, after subtracting the window mean
volatile uint16_t micPeakToPeak;
volatile uint32_t micSamples;
volatile uint32_t micWindows;
volatile uint32_t micClippedSamples;
volatile uint32_t micMissedWindows;
volatile uint32_t micAdcOverruns;
volatile uint32_t micTriggerOverruns;

static void recordSample(uint16_t raw)
{
    if (raw == 0 || raw == adcMax) ++micClippedSamples;
    int32_t corrected = (int32_t)raw + adcOffset;
    if (corrected < 0) corrected = 0;
    if (corrected > adcMax) corrected = adcMax;
    uint32_t value = (uint32_t)corrected;
    collecting.sum += value;
    collecting.sumSquares += value * value;
    if (value < collecting.minimum) collecting.minimum = value;
    if (value > collecting.maximum) collecting.maximum = value;
    ++micSamples;
    if (++samplesInWindow == windowSamples) {
        if (windowReady) ++micMissedWindows;
        completed = collecting;
        collecting = (MicWindow){.minimum = adcMax};
        samplesInWindow = 0;
        windowReady = true;
        __SEV();
    }
}

int main(void)
{
    SYSCFG_DL_init();
    runAeqFitCheck();
    adcOffset = DL_ADC12_getADCOffsetCalibration(ADC12_0_ADCMEM_0_REF_VOLTAGE_V);
    NVIC_EnableIRQ(ADC12_0_INST_INT_IRQN);
    DL_TimerG_startCounter(TIMER_0_INST);
    for (;;) {
        while (!windowReady) __WFE();
        __disable_irq();
        MicWindow window = completed;
        windowReady = false;
        __enable_irq();
        // Subtract the bias in integer arithmetic before converting to float.
        uint64_t varianceNumerator = (uint64_t)window.sumSquares * windowSamples
                                   - (uint64_t)window.sum * window.sum;
        micRms = sqrtf((float)varianceNumerator) / windowSamples;
        micMean = (float)window.sum / windowSamples;
        micPeakToPeak = window.maximum - window.minimum;
        ledBrightness = updateLedBrightness(micRms, windowSamples);
        ledDuty = (uint16_t)(ledBrightness * 256.0f + 0.5f);
        ++micWindows;
    }
}

void ADC12_0_INST_IRQHandler(void)
{
    switch (DL_ADC12_getPendingInterrupt(ADC12_0_INST)) {
        case DL_ADC12_IIDX_MEM0_RESULT_LOADED:
            // LED1 sinks current: PA22 low is the illuminated part of each pulse.
            ledAccumulator += ledDuty;
            if (ledAccumulator >= 256) {
                ledAccumulator -= 256;
                DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_PIN);
            } else {
                DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_PIN);
            }
            recordSample(DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_0));
            break;
        case DL_ADC12_IIDX_OVERFLOW:
            ++micAdcOverruns;
            break;
        case DL_ADC12_IIDX_TRIG_OVERFLOW:
            ++micTriggerOverruns;
            break;
        default:
            break;
    }
}
