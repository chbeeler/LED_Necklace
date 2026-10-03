#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// Binary analog-microphone stream:
//   START
//   sequence:u16, timestamp_us:u32, sample_count:u8,
//   (raw:u16, filtered:u16) * sample_count, crc16:u16
//   END
// All multi-byte values are little-endian. Bytes equal to START, END or ESCAPE
// in the frame body are emitted as ESCAPE followed by (byte XOR 0x20).

static const uint8_t ANALOG_MIC_PIN = PIN_A1; // XIAO A1 = nRF52840 P0.03
static const uint8_t ANALOG_MIC_ADC_BITS = 12;
static const uint8_t ANALOG_MIC_LP_SHIFT = 4; // alpha = 1/16
static const uint32_t SAMPLE_RATE_HZ = 8000;
static const uint32_t SAMPLE_PERIOD_US = 1000000UL / SAMPLE_RATE_HZ;
static const uint8_t SAMPLES_PER_BLOCK = 128;

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

static uint16_t rawSamples[SAMPLES_PER_BLOCK];
static uint16_t filteredSamples[SAMPLES_PER_BLOCK];
static uint8_t sampleIndex = 0;
static uint32_t blockTimestampUs = 0;
static uint16_t blockSequence = 0;
static int32_t filteredQ8 = 0;
static bool filterInitialized = false;
static bool serialWasConnected = false;
static uint32_t nextSampleUs = 0;

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

static void queueCompletedBlock()
{
  uint16_t sequence = blockSequence++;
  if (txQueued >= TX_QUEUE_SIZE) {
    // The sequence still advances so the receiver can report this lost block.
    return;
  }

  uint8_t body[FRAME_BODY_SIZE];
  size_t bodyLength = 0;
  appendU16Le(body, bodyLength, sequence);
  appendU32Le(body, bodyLength, blockTimestampUs);
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

static void captureOneSample(uint32_t scheduledTimestampUs)
{
  if (sampleIndex == 0) blockTimestampUs = scheduledTimestampUs;

  int raw = analogRead(ANALOG_MIC_PIN);
  int32_t targetQ8 = (int32_t)raw << 8;
  if (!filterInitialized) {
    filteredQ8 = targetQ8;
    filterInitialized = true;
  } else {
    filteredQ8 += (targetQ8 - filteredQ8) >> ANALOG_MIC_LP_SHIFT;
  }

  rawSamples[sampleIndex] = (uint16_t)raw;
  filteredSamples[sampleIndex] = (uint16_t)((filteredQ8 + 128) >> 8);
  if (++sampleIndex == SAMPLES_PER_BLOCK) {
    sampleIndex = 0;
    queueCompletedBlock();
  }
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

static void resetStream()
{
  sampleIndex = 0;
  blockSequence = 0;
  filterInitialized = false;
  txReadIndex = 0;
  txWriteIndex = 0;
  txQueued = 0;
  nextSampleUs = micros() + SAMPLE_PERIOD_US;
}

void setup()
{
  Serial.begin(921600);
  pinMode(ANALOG_MIC_PIN, INPUT);
  analogReference(AR_DEFAULT);
  analogReadResolution(ANALOG_MIC_ADC_BITS);
}

void loop()
{
  bool connected = (bool)Serial;
  if (!connected) {
    serialWasConnected = false;
    return;
  }
  if (!serialWasConnected) {
    serialWasConnected = true;
    resetStream();
  }

  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextSampleUs) >= 0) {
    uint32_t scheduledTimestampUs = nextSampleUs;
    nextSampleUs += SAMPLE_PERIOD_US;
    captureOneSample(scheduledTimestampUs);

    // Do not produce a burst of fake back-to-back samples after a long stall.
    nowUs = micros();
    if ((int32_t)(nowUs - nextSampleUs) >= (int32_t)SAMPLE_PERIOD_US) {
      nextSampleUs = nowUs + SAMPLE_PERIOD_US;
    }
  }

  serviceSerialTransmit();
}
