#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <nrf.h>

// Binary analog-microphone stream:
//   START
//   sequence:u16, timestamp_us:u32, sample_count:u8,
//   (raw:u16, filtered:u16) * sample_count, crc16:u16
//   END
// All multi-byte values are little-endian. Bytes equal to START, END or ESCAPE
// in the frame body are emitted as ESCAPE followed by (byte XOR 0x20).

static const uint8_t ANALOG_MIC_PIN = PIN_A1; // XIAO A1 = nRF52840 P0.03 / AIN1
static const uint8_t ANALOG_MIC_LP_SHIFT = 4; // alpha = 1/16
static const uint8_t ANALOG_MIC_ADC_BITS = 14;
static const int32_t ANALOG_MIC_ADC_MAX = (1L << ANALOG_MIC_ADC_BITS) - 1L;
static const uint32_t SAMPLE_RATE_HZ = 8000;
static const uint8_t SAMPLES_PER_BLOCK = 128;
static const uint32_t BLOCK_DURATION_US =
    1000000UL * SAMPLES_PER_BLOCK / SAMPLE_RATE_HZ;

// The SAADC local timer runs from 16 MHz. 2000 ticks gives exactly 8 kHz.
static const uint16_t SAADC_SAMPLE_TIMER_CC = 16000000UL / SAMPLE_RATE_HZ;
static_assert(16000000UL % SAMPLE_RATE_HZ == 0,
              "SAADC sample rate must divide 16 MHz exactly");
static_assert(SAADC_SAMPLE_TIMER_CC >= 80 && SAADC_SAMPLE_TIMER_CC <= 2047,
              "SAADC local-timer value is outside the hardware range");

static const uint8_t ADC_BUFFER_COUNT = 4;
static const uint8_t ADC_DISCARD_BUFFER = 0xFF;

enum AdcBufferState : uint8_t {
  ADC_BUFFER_FREE,
  ADC_BUFFER_FILLING,
  ADC_BUFFER_READY,
  ADC_BUFFER_PROCESSING,
};

static const uint8_t FRAME_START = 0x7E;
static const uint8_t FRAME_END = 0x7F;
static const uint8_t FRAME_ESCAPE = 0x7D;
static const uint8_t FRAME_ESCAPE_XOR = 0x20;
static const uint8_t TX_QUEUE_SIZE = 3;
static const size_t FRAME_BODY_SIZE =
    2U + 4U + 1U + (size_t)SAMPLES_PER_BLOCK * 4U + 2U;
static const size_t MAX_ENCODED_FRAME_SIZE = 2U + FRAME_BODY_SIZE * 2U;

struct EncodedFrame {
  uint8_t data[MAX_ENCODED_FRAME_SIZE];
  size_t length;
  size_t sent;
};

// EasyDMA writes directly into these buffers. The ISR only rotates buffers and
// timestamps them; filtering and framing remain in the normal Arduino task.
alignas(4) static int16_t adcBuffers[ADC_BUFFER_COUNT][SAMPLES_PER_BLOCK];
alignas(4) static int16_t adcDiscardBuffer[SAMPLES_PER_BLOCK];
static volatile uint8_t adcBufferState[ADC_BUFFER_COUNT];
static volatile uint16_t adcBufferSequence[ADC_BUFFER_COUNT];
static volatile uint32_t adcBufferTimestampUs[ADC_BUFFER_COUNT];
static volatile uint8_t completedBufferQueue[ADC_BUFFER_COUNT];
static volatile uint8_t completedQueueRead = 0;
static volatile uint8_t completedQueueWrite = 0;
static volatile uint8_t completedQueueCount = 0;
static volatile uint8_t activeAdcBuffer = ADC_DISCARD_BUFFER;
static volatile uint16_t nextAdcSequence = 0;
static volatile uint32_t activeBlockTimestampUs = 0;
static volatile uint32_t adcOverrunBlocks = 0;
static bool adcRunning = false;

static uint16_t rawSamples[SAMPLES_PER_BLOCK];
static uint16_t filteredSamples[SAMPLES_PER_BLOCK];
static int32_t filteredQ8 = 0;
static bool filterInitialized = false;
static bool serialWasConnected = false;

static EncodedFrame txQueue[TX_QUEUE_SIZE];
static uint8_t txReadIndex = 0;
static uint8_t txWriteIndex = 0;
static uint8_t txQueued = 0;

static void appendU16Le(uint8_t *buffer, size_t &offset, uint16_t value)
{
  buffer[offset++] = (uint8_t)(value & 0xFFU);
  buffer[offset++] = (uint8_t)(value >> 8);
}

static void appendU32Le(uint8_t *buffer, size_t &offset, uint32_t value)
{
  buffer[offset++] = (uint8_t)(value & 0xFFUL);
  buffer[offset++] = (uint8_t)((value >> 8) & 0xFFUL);
  buffer[offset++] = (uint8_t)((value >> 16) & 0xFFUL);
  buffer[offset++] = (uint8_t)((value >> 24) & 0xFFUL);
}

static uint16_t crc16CcittFalse(const uint8_t *data, size_t length)
{
  uint16_t crc = 0xFFFFU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                            : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static void appendEscaped(EncodedFrame &frame, uint8_t value)
{
  if (value == FRAME_START || value == FRAME_END || value == FRAME_ESCAPE) {
    frame.data[frame.length++] = FRAME_ESCAPE;
    frame.data[frame.length++] = value ^ FRAME_ESCAPE_XOR;
  } else {
    frame.data[frame.length++] = value;
  }
}

static void queueCompletedBlock(uint16_t sequence, uint32_t timestampUs)
{
  if (txQueued >= TX_QUEUE_SIZE) {
    // Sequence numbers originate in the ADC ISR, so the receiver will see a
    // gap if USB transmission cannot keep up.
    return;
  }

  uint8_t body[FRAME_BODY_SIZE];
  size_t bodyLength = 0;
  appendU16Le(body, bodyLength, sequence);
  appendU32Le(body, bodyLength, timestampUs);
  body[bodyLength++] = SAMPLES_PER_BLOCK;
  for (uint8_t i = 0; i < SAMPLES_PER_BLOCK; ++i) {
    appendU16Le(body, bodyLength, rawSamples[i]);
    appendU16Le(body, bodyLength, filteredSamples[i]);
  }
  uint16_t crc = crc16CcittFalse(body, bodyLength);
  appendU16Le(body, bodyLength, crc);

  EncodedFrame &frame = txQueue[txWriteIndex];
  frame.length = 0;
  frame.sent = 0;
  frame.data[frame.length++] = FRAME_START;
  for (size_t i = 0; i < bodyLength; ++i) appendEscaped(frame, body[i]);
  frame.data[frame.length++] = FRAME_END;

  txWriteIndex = (uint8_t)((txWriteIndex + 1U) % TX_QUEUE_SIZE);
  ++txQueued;
}

static void processAdcBlock(uint8_t bufferIndex)
{
  for (uint8_t i = 0; i < SAMPLES_PER_BLOCK; ++i) {
    int32_t raw = adcBuffers[bufferIndex][i];
    if (raw < 0) raw = 0;
    if (raw > ANALOG_MIC_ADC_MAX) raw = ANALOG_MIC_ADC_MAX;

    int32_t targetQ8 = raw << 8;
    if (!filterInitialized) {
      filteredQ8 = targetQ8;
      filterInitialized = true;
    } else {
      filteredQ8 += (targetQ8 - filteredQ8) >> ANALOG_MIC_LP_SHIFT;
    }

    rawSamples[i] = (uint16_t)raw;
    filteredSamples[i] = (uint16_t)((filteredQ8 + 128) >> 8);
  }

  queueCompletedBlock(adcBufferSequence[bufferIndex],
                      adcBufferTimestampUs[bufferIndex]);
}

static void serviceSerialTransmit()
{
  if (txQueued == 0) return;
  int available = Serial.availableForWrite();
  if (available <= 0) return;

  EncodedFrame &frame = txQueue[txReadIndex];
  size_t remaining = frame.length - frame.sent;
  size_t amount = remaining < (size_t)available ? remaining : (size_t)available;
  size_t written = Serial.write(frame.data + frame.sent, amount);
  frame.sent += written;
  if (frame.sent == frame.length) {
    txReadIndex = (uint8_t)((txReadIndex + 1U) % TX_QUEUE_SIZE);
    --txQueued;
  }
}

static uint8_t findFreeAdcBuffer()
{
  for (uint8_t i = 0; i < ADC_BUFFER_COUNT; ++i) {
    if (adcBufferState[i] == ADC_BUFFER_FREE) return i;
  }
  return ADC_DISCARD_BUFFER;
}

extern "C" void SAADC_IRQHandler(void)
{
  if (!NRF_SAADC->EVENTS_END) return;
  NRF_SAADC->EVENTS_END = 0;

  uint8_t completed = activeAdcBuffer;
  uint16_t completedSequence = nextAdcSequence++;
  uint32_t completedTimestamp = activeBlockTimestampUs;
  activeBlockTimestampUs += BLOCK_DURATION_US;

  if (completed != ADC_DISCARD_BUFFER) {
    adcBufferSequence[completed] = completedSequence;
    adcBufferTimestampUs[completed] = completedTimestamp;
    adcBufferState[completed] = ADC_BUFFER_READY;
    completedBufferQueue[completedQueueWrite] = completed;
    completedQueueWrite = (uint8_t)((completedQueueWrite + 1U) % ADC_BUFFER_COUNT);
    ++completedQueueCount;
  } else {
    ++adcOverrunBlocks;
  }

  uint8_t next = findFreeAdcBuffer();
  activeAdcBuffer = next;
  if (next == ADC_DISCARD_BUFFER) {
    NRF_SAADC->RESULT.PTR = (uint32_t)adcDiscardBuffer;
  } else {
    adcBufferState[next] = ADC_BUFFER_FILLING;
    NRF_SAADC->RESULT.PTR = (uint32_t)adcBuffers[next];
  }
  NRF_SAADC->RESULT.MAXCNT = SAMPLES_PER_BLOCK;
  NRF_SAADC->TASKS_START = 1;
}

static bool takeCompletedAdcBuffer(uint8_t &bufferIndex)
{
  NVIC_DisableIRQ(SAADC_IRQn);
  if (completedQueueCount == 0) {
    NVIC_EnableIRQ(SAADC_IRQn);
    return false;
  }

  bufferIndex = completedBufferQueue[completedQueueRead];
  completedQueueRead = (uint8_t)((completedQueueRead + 1U) % ADC_BUFFER_COUNT);
  --completedQueueCount;
  adcBufferState[bufferIndex] = ADC_BUFFER_PROCESSING;
  NVIC_EnableIRQ(SAADC_IRQn);
  return true;
}

static void releaseAdcBuffer(uint8_t bufferIndex)
{
  // A byte store is atomic on Cortex-M4. If END happens immediately before
  // this store, the ISR may discard one block, but it cannot reuse a buffer
  // while it is being processed.
  adcBufferState[bufferIndex] = ADC_BUFFER_FREE;
}

static void resetStreamState()
{
  filterInitialized = false;
  filteredQ8 = 0;
  txReadIndex = 0;
  txWriteIndex = 0;
  txQueued = 0;

  completedQueueRead = 0;
  completedQueueWrite = 0;
  completedQueueCount = 0;
  nextAdcSequence = 0;
  adcOverrunBlocks = 0;
  for (uint8_t i = 0; i < ADC_BUFFER_COUNT; ++i) {
    adcBufferState[i] = ADC_BUFFER_FREE;
  }
}

static void startAdcSampling()
{
  resetStreamState();

  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled << SAADC_ENABLE_ENABLE_Pos;
  // Internal 0.6 V reference with gain 1/2 gives a 0..1.2 V single-ended
  // range. Four-conversion burst oversampling produces one 14-bit result at
  // every 8 kHz timer tick.
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_14bit;
  NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Over4x;
  NRF_SAADC->CH[0].CONFIG =
      (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
      (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
      (SAADC_CH_CONFIG_GAIN_Gain1_2 << SAADC_CH_CONFIG_GAIN_Pos) |
      (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
      (SAADC_CH_CONFIG_TACQ_3us << SAADC_CH_CONFIG_TACQ_Pos) |
      (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |
      (SAADC_CH_CONFIG_BURST_Enabled << SAADC_CH_CONFIG_BURST_Pos);
  NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput1;
  NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;
  for (uint8_t channel = 1; channel < 8; ++channel) {
    NRF_SAADC->CH[channel].PSELP = SAADC_CH_PSELP_PSELP_NC;
    NRF_SAADC->CH[channel].PSELN = SAADC_CH_PSELN_PSELN_NC;
  }

  NRF_SAADC->SAMPLERATE =
      (SAADC_SAMPLERATE_MODE_Timers << SAADC_SAMPLERATE_MODE_Pos) |
      ((uint32_t)SAADC_SAMPLE_TIMER_CC << SAADC_SAMPLERATE_CC_Pos);

  // Offset calibration is done once per connection before the timer starts.
  NRF_SAADC->EVENTS_CALIBRATEDONE = 0;
  NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
  while (!NRF_SAADC->EVENTS_CALIBRATEDONE) {}
  NRF_SAADC->EVENTS_CALIBRATEDONE = 0;

  activeAdcBuffer = 0;
  adcBufferState[0] = ADC_BUFFER_FILLING;
  activeBlockTimestampUs = micros();
  NRF_SAADC->RESULT.PTR = (uint32_t)adcBuffers[0];
  NRF_SAADC->RESULT.MAXCNT = SAMPLES_PER_BLOCK;
  NRF_SAADC->EVENTS_STARTED = 0;
  NRF_SAADC->EVENTS_END = 0;
  NRF_SAADC->INTENSET = SAADC_INTENSET_END_Set << SAADC_INTENSET_END_Pos;
  NVIC_ClearPendingIRQ(SAADC_IRQn);
  NVIC_SetPriority(SAADC_IRQn, 6);
  NVIC_EnableIRQ(SAADC_IRQn);

  // START only latches the EasyDMA buffer. In local-timer mode one SAMPLE
  // task is still required to start the periodic conversions; after that the
  // timer runs until TASKS_STOP is issued.
  NRF_SAADC->TASKS_START = 1;
  while (!NRF_SAADC->EVENTS_STARTED) {}
  NRF_SAADC->EVENTS_STARTED = 0;
  NRF_SAADC->TASKS_SAMPLE = 1;
  adcRunning = true;
}

static void stopAdcSampling()
{
  if (!adcRunning) return;

  NVIC_DisableIRQ(SAADC_IRQn);
  NRF_SAADC->INTENCLR = SAADC_INTENCLR_END_Clear << SAADC_INTENCLR_END_Pos;
  NRF_SAADC->EVENTS_STOPPED = 0;
  NRF_SAADC->TASKS_STOP = 1;
  while (!NRF_SAADC->EVENTS_STOPPED) {}
  NRF_SAADC->EVENTS_STOPPED = 0;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled << SAADC_ENABLE_ENABLE_Pos;
  activeAdcBuffer = ADC_DISCARD_BUFFER;
  adcRunning = false;
}

void setup()
{
  Serial.begin(921600);
  pinMode(ANALOG_MIC_PIN, INPUT);
}

void loop()
{
  bool connected = (bool)Serial;
  if (!connected) {
    if (serialWasConnected) stopAdcSampling();
    serialWasConnected = false;
    return;
  }
  if (!serialWasConnected) {
    serialWasConnected = true;
    startAdcSampling();
  }

  serviceSerialTransmit();

  uint8_t completedBuffer;
  if (takeCompletedAdcBuffer(completedBuffer)) {
    processAdcBlock(completedBuffer);
    releaseAdcBuffer(completedBuffer);
  }

  serviceSerialTransmit();
}
