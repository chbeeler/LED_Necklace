#include "audio.h"
#include "board.h"
#include "ti_msp_dl_config.h"
#include <math.h>

#if AUDIO_DMA_CHAN_ID != 0
#error "The audio IRQ handler requires DMA channel 0"
#endif

enum { BLOCK_WORDS = 64, BLOCK_FRAMES = 32, ADC_MAX = 4095 };
// Windows end on fixed DMA block numbers, so the interrupt can apply a new
// LED duty exactly between two windows.
enum { WINDOW_BLOCKS = AUDIO_WINDOW_PAIRS / BLOCK_FRAMES };
typedef struct {
    uint32_t sum;
    uint64_t sumSquares; // 256 sums of three conversions exceed 32 bits.
    uint16_t minimum;
    uint16_t maximum;
} Accumulator;
enum { AMP_MAX = 3 * ADC_MAX };

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
static Accumulator amp = {.minimum = AMP_MAX};
static bool ampAverage = true;
static Accumulator test[AUDIO_TEST_LEVELS];
static bool testMode;
static uint16_t accumulatedPairs;
static uint32_t lastBlock;

static void resetAccumulators(void)
{
    for (unsigned i = 0; i < AUDIO_TEST_LEVELS; ++i)
        test[i] = (Accumulator){.minimum = AMP_MAX};
    opa = (Accumulator){.minimum = ADC_MAX};
    raw = (Accumulator){.minimum = ADC_MAX};
    gpamp = (Accumulator){.minimum = ADC_MAX};
    amp = (Accumulator){.minimum = AMP_MAX};
    accumulatedPairs = 0;
}

void audioSetTestMode(bool enable)
{
    testMode = enable;
    resetAccumulators();
}

void audioSetAveraging(bool averageThree)
{
    ampAverage = averageThree;
    // The window in progress is now short and will be discarded.
    resetAccumulators();
}
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

// Each accumulated value was the sum of `conversions` ADC results; the level
// is reported in ADC counts.
static AudioLevel finish(const Accumulator *acc, unsigned conversions)
{
    const uint64_t numerator = acc->sumSquares * AUDIO_WINDOW_PAIRS
                             - (uint64_t)acc->sum * acc->sum;
    const float scale = (float)(AUDIO_WINDOW_PAIRS * conversions);
    return (AudioLevel){
        .mean = (float)acc->sum / scale,
        .rms = sqrtf((float)numerator) / scale,
        .minimum = (uint16_t)(acc->minimum / conversions),
        .maximum = (uint16_t)(acc->maximum / conversions),
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
        resetAccumulators();
        if (captureActive) captureCount = 0;
    }
    lastBlock = sequence;
    if (captureActive) captureBlock();
    for (unsigned i = 0; i < BLOCK_WORDS; i += 2) {
        const uint16_t slot0 = snapshot[i] & ADC_MAX;
        const uint16_t slot2 = snapshot[i + 1] & ADC_MAX;
        const uint16_t slot3 = (snapshot[i + 1] >> 16) & ADC_MAX;
        accumulate(&opa, slot0);
        accumulate(&raw, (uint16_t)((snapshot[i] >> 16) & ADC_MAX));
        accumulate(&gpamp, slot2);
        accumulate(&amp, ampAverage ? slot0 + slot2 + slot3 : slot0);
        if (testMode) {
            const uint16_t slot1 = (snapshot[i] >> 16) & ADC_MAX;
            accumulate(&test[0], slot0);
            accumulate(&test[1], slot1);
            accumulate(&test[2], slot0 + slot1);
            accumulate(&test[3], slot2);
            accumulate(&test[4], slot3);
            accumulate(&test[5], slot2 + slot3);
        }
    }
    accumulatedPairs += BLOCK_FRAMES;
    if (sequence % WINDOW_BLOCKS) return false;
    // A window that lost blocks is discarded instead of reported short.
    const bool complete = accumulatedPairs == AUDIO_WINDOW_PAIRS;
    if (complete) {
        window->opa = finish(&opa, 1);
        window->raw = finish(&raw, 1);
        window->gpamp = finish(&gpamp, 1);
        window->amp = finish(&amp, ampAverage ? 3 : 1);
        for (unsigned i = 0; testMode && i < AUDIO_TEST_LEVELS; ++i)
            window->testRms[i] = finish(&test[i], i % 3 == 2 ? 2 : 1).rms;
        window->timeMs = sequence * (BLOCK_FRAMES * 1000 / AUDIO_SAMPLE_RATE);
        ++micWindows;
    }
    resetAccumulators();
    return complete;
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
    if (micDmaBlocks % WINDOW_BLOCKS == 0) boardLatchLed();
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
