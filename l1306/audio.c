#include "audio.h"
#include "ti_msp_dl_config.h"
#include <math.h>

#if AUDIO_DMA_CHAN_ID != 0
#error "The audio IRQ handler requires DMA channel 0"
#endif

enum { BLOCK_WORDS = 64, BLOCK_FRAMES = 32, ADC_MAX = 4095 };
typedef struct {
    uint32_t sum;
    uint32_t sumSquares;
    uint16_t minimum;
    uint16_t maximum;
} Accumulator;

// Two FIFO words per frame: OPA/raw, then GPAMP/discarded GPAMP repeat.
// The ISR rearms the alternate buffer before processing the completed block.
static volatile uint32_t buffers[2][BLOCK_WORDS];
static uint32_t snapshot[BLOCK_WORDS];
static volatile uint8_t activeBuffer;
static volatile uint8_t readyBuffer;
static volatile bool blockReady;
static Accumulator opa = {.minimum = ADC_MAX};
static Accumulator raw = {.minimum = ADC_MAX};
static Accumulator gpamp = {.minimum = ADC_MAX};
static uint16_t accumulatedPairs;
static uint32_t lastBlock;
static uint16_t capture[AUDIO_CAPTURE_SAMPLES];
static unsigned captureCount;
static bool captureActive;
static AudioCaptureMode captureMode;

void audioCaptureStart(AudioCaptureMode mode)
{
    captureMode = mode;
    captureCount = 0;
    captureActive = true;
}

void audioCaptureCancel(void)
{
    captureCount = 0;
    captureActive = false;
}

const uint16_t *audioCaptureResult(void)
{
    return captureCount == AUDIO_CAPTURE_SAMPLES ? capture : 0;
}

static void captureBlock(void)
{
    for (unsigned i = 0; i < BLOCK_WORDS; i += 2) {
        if (captureMode == AUDIO_CAPTURE_ALL) {
            capture[captureCount++] = snapshot[i] & ADC_MAX;
            capture[captureCount++] = (snapshot[i] >> 16) & ADC_MAX;
            capture[captureCount++] = snapshot[i + 1] & ADC_MAX;
            capture[captureCount++] = (snapshot[i + 1] >> 16) & ADC_MAX;
        } else {
            const unsigned word = i + (captureMode >= AUDIO_CAPTURE_GPAMP);
            const unsigned shift = (captureMode & 1) * 16;
            capture[captureCount++] = (snapshot[word] >> shift) & ADC_MAX;
        }
    }
    if (captureCount == AUDIO_CAPTURE_SAMPLES) captureActive = false;
}

volatile uint32_t micDmaBlocks;
volatile uint32_t micMissedBlocks;
volatile uint32_t micAdcOverruns;
volatile uint32_t micTriggerOverruns;
volatile uint32_t micWindows;

static void armBuffer(uint8_t index)
{
    DL_DMA_setDestAddr(DMA, AUDIO_DMA_CHAN_ID, (uint32_t)buffers[index]);
    DL_DMA_setTransferSize(DMA, AUDIO_DMA_CHAN_ID, BLOCK_WORDS);
    DL_DMA_enableChannel(DMA, AUDIO_DMA_CHAN_ID);
}

void audioStart(void)
{
    // This also covers microphone, feedback capacitor and VREF settling.
    delay_cycles(CPUCLK_FREQ / 2);
    DL_DMA_setSrcAddr(DMA, AUDIO_DMA_CHAN_ID,
                     (uint32_t)DL_ADC12_getFIFOAddress(AUDIO_ADC_INST));
    DL_DMA_clearInterruptStatus(DMA, DL_DMA_INTERRUPT_CHANNEL0);
    DL_DMA_enableInterrupt(DMA, DL_DMA_INTERRUPT_CHANNEL0);
    NVIC_SetPriority(DMA_INT_IRQn, 0);
    NVIC_EnableIRQ(DMA_INT_IRQn);
    NVIC_EnableIRQ(AUDIO_ADC_INST_INT_IRQN);
    armBuffer(0);
    DL_TimerG_setCoreHaltBehavior(AUDIO_TIMER_INST, DL_TIMER_CORE_HALT_IMMEDIATE);
    DL_TimerG_startCounter(AUDIO_TIMER_INST);
}

static void accumulate(Accumulator *acc, uint16_t value)
{
    acc->sum += value;
    acc->sumSquares += (uint32_t)value * value;
    if (value < acc->minimum) acc->minimum = value;
    if (value > acc->maximum) acc->maximum = value;
}

static AudioLevel finish(const Accumulator *acc)
{
    // 256 * 4095^2 fits in uint32_t; the variance numerator needs 64 bits.
    const uint64_t numerator = (uint64_t)acc->sumSquares * AUDIO_WINDOW_PAIRS
                             - (uint64_t)acc->sum * acc->sum;
    return (AudioLevel){
        .mean = (float)acc->sum / AUDIO_WINDOW_PAIRS,
        .rms = sqrtf((float)numerator) / AUDIO_WINDOW_PAIRS,
        .minimum = acc->minimum,
        .maximum = acc->maximum,
    };
}

bool audioPollWindow(AudioWindow *window)
{
    const uint32_t mask = __get_PRIMASK();
    __disable_irq();
    if (!blockReady) {
        __set_PRIMASK(mask);
        return false;
    }
    const uint32_t sequence = micDmaBlocks;
    // DMA writes the other buffer. The masked ISR cannot recycle this one
    // during the short copy; no floating-point work runs with IRQs masked.
    for (unsigned i = 0; i < BLOCK_WORDS; ++i)
        snapshot[i] = buffers[readyBuffer][i];
    blockReady = false;
    __set_PRIMASK(mask);

    if (sequence != lastBlock + 1) {
        opa = (Accumulator){.minimum = ADC_MAX};
        raw = (Accumulator){.minimum = ADC_MAX};
        gpamp = (Accumulator){.minimum = ADC_MAX};
        accumulatedPairs = 0;
        if (captureActive) captureCount = 0;
    }
    lastBlock = sequence;
    if (captureActive) captureBlock();
    for (unsigned i = 0; i < BLOCK_WORDS; i += 2) {
        accumulate(&opa, (uint16_t)(snapshot[i] & ADC_MAX));
        accumulate(&raw, (uint16_t)((snapshot[i] >> 16) & ADC_MAX));
        accumulate(&gpamp, (uint16_t)(snapshot[i+1] & ADC_MAX));
    }
    accumulatedPairs += BLOCK_FRAMES;
    if (accumulatedPairs < AUDIO_WINDOW_PAIRS) return false;
    window->opa = finish(&opa);
    window->raw = finish(&raw);
    window->gpamp = finish(&gpamp);
    window->timeMs = sequence * (BLOCK_FRAMES * 1000 / AUDIO_SAMPLE_RATE);
    opa = (Accumulator){.minimum = ADC_MAX};
    raw = (Accumulator){.minimum = ADC_MAX};
    gpamp = (Accumulator){.minimum = ADC_MAX};
    accumulatedPairs = 0;
    ++micWindows;
    return true;
}

void DMA_IRQHandler(void)
{
    if (DL_DMA_getPendingInterrupt(DMA) != DL_DMA_EVENT_IIDX_DMACH0) return;
    const uint8_t completed = activeBuffer;
    activeBuffer ^= 1;
    armBuffer(activeBuffer);
    if (blockReady) ++micMissedBlocks;
    readyBuffer = completed;
    ++micDmaBlocks;
    blockReady = true;
    __SEV();
}

void AUDIO_ADC_INST_IRQHandler(void)
{
    switch (DL_ADC12_getPendingInterrupt(AUDIO_ADC_INST)) {
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
