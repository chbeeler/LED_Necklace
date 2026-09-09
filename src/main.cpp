#include <Arduino.h>
//#include "board_pins.h"
#define USE_PDM
#ifdef USE_PDM
#include <PDM.h>
#endif

#ifndef ENABLE_NFC
#define ENABLE_NFC 0
#endif

#if ENABLE_NFC
extern "C" {
#include "nfc_t2t_lib.h"
}
#endif

#include "battery.h"
#include "ble_control.h"

#include <Adafruit_SPIFlash.h>
#include <LSM6DS3.h>
#include <bluefruit.h>
#include <math.h>
#include <string.h>
//#include <nrf52840.h>

#define SERIAL_OUTPUT
#define USE_BLE

// Potentiometer wiring: D4 and D6 are briefly driven as the two ends, D5 is
// the wiper.  Keep these calibration values within the 12-bit ADC range.
#define POT_PIN_LOW             D4
#define POT_PIN_WIPER           PIN_A5
#define POT_PIN_HIGH            D6
#define POT_ADC_RESOLUTION_BITS  12
#define POT_REVERSE_DIRECTION   0  // Set to 1 to reverse clockwise direction.
#define POT_MIN_READING         20  // Measured ADC value at the low end stop.
#define POT_MAX_READING         3700 // Measured ADC value at the high end stop.
#define POT_HYSTERESIS          10  // Minimum calibrated ADC-count change to accept.
#define POT_READ_PERIOD_MS      250UL
#define POT_SAMPLE_COUNT        4   // Supported range: 2..4 samples per reading.

// Firefly cycle timing.
#define FIREFLY_FADE_IN_MS       350UL
#define FIREFLY_ON_MS              20UL
#define FIREFLY_FADE_OUT_MS      900UL
#define FIREFLY_NORMAL_PAUSE_MIN_MS       1500UL
#define FIREFLY_NORMAL_PAUSE_MAX_MS       3500UL
#define FIREFLY_DOUBLE_GAP_MIN_MS           350UL
#define FIREFLY_DOUBLE_GAP_MAX_MS           700UL
#define FIREFLY_POST_DOUBLE_PAUSE_MIN_MS   1800UL
#define FIREFLY_POST_DOUBLE_PAUSE_MAX_MS   4000UL
#define FIREFLY_LONG_PAUSE_MIN_MS          4000UL
#define FIREFLY_LONG_PAUSE_MAX_MS          7000UL
#define FIREFLY_DOUBLE_FLASH_PERCENT         20
#define FIREFLY_LONG_PAUSE_PERCENT           10
#define FIREFLY_SECOND_FLASH_MIN_PERCENT      60
#define FIREFLY_SECOND_FLASH_MAX_PERCENT      85
#define FIREFLY_GAMMA              2.2f

#if POT_SAMPLE_COUNT < 2 || POT_SAMPLE_COUNT > 4
#error "POT_SAMPLE_COUNT must be between 2 and 4"
#endif

#if FIREFLY_FADE_IN_MS == 0 || FIREFLY_FADE_OUT_MS == 0
#error "Firefly fade durations must be greater than zero"
#endif

#if FIREFLY_NORMAL_PAUSE_MIN_MS > FIREFLY_NORMAL_PAUSE_MAX_MS || \
    FIREFLY_DOUBLE_GAP_MIN_MS > FIREFLY_DOUBLE_GAP_MAX_MS || \
    FIREFLY_POST_DOUBLE_PAUSE_MIN_MS > FIREFLY_POST_DOUBLE_PAUSE_MAX_MS || \
    FIREFLY_LONG_PAUSE_MIN_MS > FIREFLY_LONG_PAUSE_MAX_MS
#error "Firefly minimum durations must not exceed their maximums"
#endif

#if FIREFLY_DOUBLE_FLASH_PERCENT + FIREFLY_LONG_PAUSE_PERCENT > 100
#error "Firefly event probabilities must not exceed 100 percent"
#endif

#if FIREFLY_SECOND_FLASH_MIN_PERCENT > FIREFLY_SECOND_FLASH_MAX_PERCENT || \
    FIREFLY_SECOND_FLASH_MAX_PERCENT > 100
#error "Firefly second-flash brightness percentages are invalid"
#endif

extern BLEUart bleuart;   // global

// ---- PWM configuration ----
const uint8_t PWM_RES_BITS = 12;    // 12-bit: 0..4095 (nRF52840 HW can do up to 15 bits)
uint16_t pwmMax = (1 << PWM_RES_BITS) - 1;

#define LED_CHANNEL 3

typedef enum {charge_current_50, charge_current_100} charge_current_setting_t;
charge_current_setting_t chargeCurrentSelection = charge_current_50;

// default number of output channels
static const char channels = 1;

// default PCM output frequency
static const int frequency = 16000;

// Buffer to read samples into, each sample is 16-bits
short sampleBuffer[512];

// Number of audio samples read
volatile int samplesRead = 0;

// ---- Beat / loudness detection variables ----

// Low-pass filtered loudness value
float filteredLoudness = 0.0f;

// How fast the filter reacts (0..1, closer to 0 = slower, more smoothing)
const float LP_ALPHA = 0.15f;

// ---- Volume min/max tracking over a 5s window ----
static const uint32_t VOL_RANGE_WINDOW_MS = 5000;
static uint16_t vol_window_min = 0xFFFF;
static uint16_t vol_window_max = 0;
static uint32_t vol_window_start_ms = 0;

static const uint8_t MODE_OFF      = 0;
static const uint8_t MODE_EQ       = 1;
static const uint8_t MODE_CONSTANT = 2;
static const uint8_t MODE_AEQ      = 3;
static const uint8_t MODE_FIREFLY  = 4;
static const uint8_t MODE_POWEROFF = 9;

unsigned long lastBatSampleMs = 0;
bool lowBattery = false;

int brightness = 100;
float sensitivityF = 8.0f;    //Scale mic loudness into LED brightness (tune this)
float autoSensitivityF = 8.0f;
float autoNoiseFloorF = 10.0f;

bool lowPowerTest = false;
volatile bool shutdownIrqSeen = false;
#if ENABLE_NFC
volatile bool nfcEffectTrigger = false;
bool nfcEffectActive = false;
uint32_t nfcEffectStartMs = 0;
#endif
uint32_t lastBleCtrlUpdateMs = 0;
uint32_t lastBleVbatUpdateMs = 0;
uint32_t lastPotReadMs = 0;
int lastPotRaw = -1;
int lastPotMode = -1;

#if ENABLE_NFC
static const uint32_t NFC_EFFECT_DURATION_MS = 4000UL;
static const float NFC_EFFECT_FREQUENCY_HZ = 0.8f;
static const int NFC_EFFECT_MIN = 5;
static const int NFC_EFFECT_MAX = 250;
#endif
static const uint32_t BLE_CTRL_UPDATE_PERIOD_MS = 20UL;
static const uint32_t BLE_VBAT_UPDATE_PERIOD_MS = 500UL;

enum FireflyPhase : uint8_t {
  FIREFLY_FADE_IN,
  FIREFLY_ON,
  FIREFLY_FADE_OUT,
  FIREFLY_PAUSE
};

static FireflyPhase fireflyPhase = FIREFLY_FADE_IN;
static uint32_t fireflyPhaseStartMs = 0;
static uint32_t fireflyPhaseDurationMs = FIREFLY_FADE_IN_MS;
static bool fireflyActive = false;
static bool fireflyIsSecondFlash = false;
static bool fireflyNextFlashIsSecond = false;
static float fireflyPeakScale = 1.0f;
static float fireflyNextPeakScale = 1.0f;

static uint32_t randomDuration(uint32_t minMs, uint32_t maxMs)
{
  return (uint32_t)random((long)minMs, (long)maxMs + 1L);
}

static void startFirefly(uint32_t nowMs)
{
  fireflyActive = true;
  fireflyPhase = FIREFLY_FADE_IN;
  fireflyPhaseStartMs = nowMs;
  fireflyPhaseDurationMs = FIREFLY_FADE_IN_MS;
  fireflyIsSecondFlash = false;
  fireflyNextFlashIsSecond = false;
  fireflyPeakScale = 1.0f;
  fireflyNextPeakScale = 1.0f;
}

static void advanceFireflyPhase()
{
  fireflyPhaseStartMs += fireflyPhaseDurationMs;

  switch (fireflyPhase) {
    case FIREFLY_FADE_IN:
      fireflyPhase = FIREFLY_ON;
      fireflyPhaseDurationMs = FIREFLY_ON_MS;
      break;
    case FIREFLY_ON:
      fireflyPhase = FIREFLY_FADE_OUT;
      fireflyPhaseDurationMs = FIREFLY_FADE_OUT_MS;
      break;
    case FIREFLY_FADE_OUT:
      fireflyPhase = FIREFLY_PAUSE;
      if (fireflyIsSecondFlash) {
        fireflyNextFlashIsSecond = false;
        fireflyNextPeakScale = 1.0f;
        fireflyPhaseDurationMs = randomDuration(
            FIREFLY_POST_DOUBLE_PAUSE_MIN_MS,
            FIREFLY_POST_DOUBLE_PAUSE_MAX_MS);
      } else {
        long eventRoll = random(100);
        if (eventRoll < FIREFLY_DOUBLE_FLASH_PERCENT) {
          fireflyNextFlashIsSecond = true;
          fireflyNextPeakScale = (float)random(
              FIREFLY_SECOND_FLASH_MIN_PERCENT,
              FIREFLY_SECOND_FLASH_MAX_PERCENT + 1) / 100.0f;
          fireflyPhaseDurationMs = randomDuration(
              FIREFLY_DOUBLE_GAP_MIN_MS,
              FIREFLY_DOUBLE_GAP_MAX_MS);
        } else {
          fireflyNextFlashIsSecond = false;
          fireflyNextPeakScale = 1.0f;
          if (eventRoll < FIREFLY_DOUBLE_FLASH_PERCENT + FIREFLY_LONG_PAUSE_PERCENT) {
            fireflyPhaseDurationMs = randomDuration(
                FIREFLY_LONG_PAUSE_MIN_MS,
                FIREFLY_LONG_PAUSE_MAX_MS);
          } else {
            fireflyPhaseDurationMs = randomDuration(
                FIREFLY_NORMAL_PAUSE_MIN_MS,
                FIREFLY_NORMAL_PAUSE_MAX_MS);
          }
        }
      }
      break;
    case FIREFLY_PAUSE:
      fireflyPhase = FIREFLY_FADE_IN;
      fireflyPhaseDurationMs = FIREFLY_FADE_IN_MS;
      fireflyIsSecondFlash = fireflyNextFlashIsSecond;
      fireflyPeakScale = fireflyNextPeakScale;
      break;
  }
}

static float smoothstep(float x)
{
  return x * x * (3.0f - 2.0f * x);
}

static void updateFirefly(uint32_t nowMs)
{
  if (!fireflyActive) startFirefly(nowMs);

  // Normally this advances at most once. The loop also catches up cleanly if
  // another operation temporarily delayed execution.
  while ((uint32_t)(nowMs - fireflyPhaseStartMs) >= fireflyPhaseDurationMs) {
    advanceFireflyPhase();
  }

  float level = 0.0f;
  if (fireflyPhase == FIREFLY_ON) {
    level = 1.0f;
  } else if (fireflyPhase == FIREFLY_FADE_IN || fireflyPhase == FIREFLY_FADE_OUT) {
    float progress = (float)(nowMs - fireflyPhaseStartMs) /
                     (float)fireflyPhaseDurationMs;
    float eased = smoothstep(progress);
    if (fireflyPhase == FIREFLY_FADE_IN) {
      // Quick nonlinear rise: sqrt(smoothstep(x)).
      level = sqrtf(eased);
    } else {
      // Softer nonlinear decay: 1 - smoothstep(x)^1.5.
      level = 1.0f - powf(eased, 1.5f);
    }
  }

  // Gamma-correct only the normalized envelope, then scale it so level 1.0
  // always maps exactly to the brightness selected over BLE.
  float pwmLevel = powf(level, FIREFLY_GAMMA);
  uint32_t ledValue = (uint32_t)(
      (float)brightness * fireflyPeakScale * pwmLevel + 0.5f);
  if (ledValue > pwmMax) ledValue = pwmMax;
  analogWrite(LED_CHANNEL, (int)ledValue);
}

static void disablePotentiometer()
{
  // Do not leave either end of the potentiometer powered between readings.
  pinMode(POT_PIN_LOW, INPUT);
  pinMode(POT_PIN_HIGH, INPUT);
  pinMode(POT_PIN_WIPER, INPUT);
}

static void updatePotentiometer(uint32_t nowMs)
{
  uint8_t potMode = bleGetPotMode();
  if ((int)potMode != lastPotMode) {
    lastPotMode = potMode;
    lastPotRaw = -1;
  }

  if (potMode == POT_MODE_DISABLED) {
    disablePotentiometer();
    return;
  }

  if ((uint32_t)(nowMs - lastPotReadMs) < POT_READ_PERIOD_MS) return;
  lastPotReadMs = nowMs;

  pinMode(POT_PIN_LOW, OUTPUT);
  digitalWrite(POT_PIN_LOW, LOW);
  pinMode(POT_PIN_HIGH, OUTPUT);
  digitalWrite(POT_PIN_HIGH, HIGH);
  pinMode(POT_PIN_WIPER, INPUT);
  delay(1); // Let the resistor divider and ADC input settle after power-up.

  // Battery measurement uses the 1.8 V range; the pot needs the 3.6 V range.
  analogReference(AR_DEFAULT);    // 3.6V range
  analogReadResolution(POT_ADC_RESOLUTION_BITS);
  analogRead(POT_PIN_WIPER); // Discard the first conversion after enabling it.

  uint32_t rawSum = 0;
  for (uint8_t i = 0; i < POT_SAMPLE_COUNT; ++i) {
    rawSum += analogRead(POT_PIN_WIPER);
  }
  int rawReading = (int)(rawSum / POT_SAMPLE_COUNT);
  disablePotentiometer();

  if (POT_MAX_READING <= POT_MIN_READING) return;
  int raw = constrain(rawReading, POT_MIN_READING, POT_MAX_READING);
  if (lastPotRaw >= 0 && abs(raw - lastPotRaw) < POT_HYSTERESIS) return;

  lastPotRaw = raw;
  int value = (raw - POT_MIN_READING) *
              ((potMode == POT_MODE_BRIGHTNESS) ? 400 : 1000) /
              (POT_MAX_READING - POT_MIN_READING);
  if (POT_REVERSE_DIRECTION) {
    value = ((potMode == POT_MODE_BRIGHTNESS) ? 400 : 1000) - value;
  }

  if (potMode == POT_MODE_BRIGHTNESS) {
    bleSetBrightnessFromPot((uint16_t)value);
  } else { // POT_MODE_EQ_SENSITIVITY
    bleSetSensitivityFromPot((uint16_t)value);
  }

  Serial.print("POT raw=");
  Serial.print(rawReading);
  Serial.print(" value=");
  Serial.print(value);
  Serial.print(" mode=");
  Serial.println(potMode);
}
#if ENABLE_NFC
static const uint8_t NFC_URI_PREFIX = 0x04; // "https://"
static const char NFC_URI_TEXT[] = "wa.me/41774321412?text=Hey%20Chris%2C%20cool%20necklace%21";

static bool buildNdefUriRecord(const char *uri_text,
                               uint8_t uri_prefix,
                               uint8_t *out_buf,
                               size_t out_buf_size,
                               size_t *out_len)
{
  if ((uri_text == nullptr) || (out_buf == nullptr) || (out_len == nullptr)) {
    return false;
  }

  size_t uri_len = strlen(uri_text);
  if ((uri_len == 0U) || ((uri_len + 1U) > 255U)) {
    return false;
  }

  size_t record_len = 5U + uri_len;
  if (record_len > out_buf_size) {
    return false;
  }

  // NDEF short record: MB|ME|SR, TNF=well-known, type='U' (URI)
  out_buf[0] = 0xD1;
  out_buf[1] = 0x01;
  out_buf[2] = (uint8_t)(uri_len + 1U); // +1 for URI prefix byte
  out_buf[3] = 0x55;                     // 'U'
  out_buf[4] = uri_prefix;
  memcpy(&out_buf[5], uri_text, uri_len);

  *out_len = record_len;
  return true;
}

static bool nfc_rc_ok(int rc)
{
  return (rc == 0) || ((uint32_t)rc == 0x0BAD0000UL);
}

static void updateNfcEffect()
{
  uint32_t elapsed_ms = millis() - nfcEffectStartMs;
  if (elapsed_ms >= NFC_EFFECT_DURATION_MS) {
    nfcEffectActive = false;
    return;
  }

  float t_sec = (float)elapsed_ms / 1000.0f;
  float phase = 2.0f * PI * NFC_EFFECT_FREQUENCY_HZ * t_sec;
  float wave = 0.5f * (sinf(phase) + 1.0f);
  int ledValue = NFC_EFFECT_MIN + (int)((float)(NFC_EFFECT_MAX - NFC_EFFECT_MIN) * wave);

  if (ledValue < 0) {
    ledValue = 0;
  } else if (ledValue > (int)pwmMax) {
    ledValue = pwmMax;
  }

  analogWrite(LED_CHANNEL, ledValue);
}

#endif

#ifdef USE_PDM
void onPDMdata();
#endif

#if ENABLE_NFC
static void nfc_cb(void *context, nfc_t2t_event_t event, const uint8_t *data, size_t data_length)
{
  (void)context;
  (void)data;
  (void)data_length;

  if (event == NFC_T2T_EVENT_DATA_READ) {
    nfcEffectTrigger = true;
  }
}
#endif

Adafruit_FlashTransport_QSPI flashTransport;
Adafruit_SPIFlash flash(&flashTransport);
LSM6DS3 myIMU; // Default is I2C mode

bool deepPowerDown(Adafruit_SPIFlash& flash, Adafruit_FlashTransport& transport) {
  uint32_t id_before = flash.getJEDECID();

  transport.begin();
  transport.runCommand(0xB9);  // SPI deep power-down command
  delay(10);

  uint32_t id_after = flash.getJEDECID();

  return (id_after == 0xFFFFFF || id_after == 0xFFFFFFFF);
}

void setupWakeUpInterrupt()
{
  // Start with LSM6DS3 in disabled to save power
  myIMU.settings.gyroEnabled = 0;
  myIMU.settings.accelEnabled = 0;
  myIMU.begin();
  //Values that use the highest low power setting (52Hz) with resulting physical tap detection parameters that are
  //close as possible to the application note values.
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x30);    // Turn on the accelerometer // ODR_XL = 52 Hz, FS_XL = ±2 g                                                           
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_TAP_CFG1, 0x8E);    // Enable interrupts and tap detection on X, Y, Z-axis
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_TAP_THS_6D, 0x8C);  // Set tap threshold
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_INT_DUR2, 0x10);    // Set Duration, Quiet and Shock time windows
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_WAKE_UP_THS, 0x80); // Single & double-tap enabled (SINGLE_DOUBLE_TAP = 1)
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_CTRL6_G, 0x10);     // High-performance operating mode disabled for accelerometer
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_MD1_CFG, 0x08);     // Double-tap interrupt driven to INT1 pin

  // Set up the sense mechanism to generate the DETECT signal to wake from system_off
  // No need to attach a handler, if just waking with the GPIO input.
  pinMode(PIN_LSM6DS3TR_C_INT1, INPUT_PULLDOWN_SENSE);
  return;
}

void updateLEDs()
{
  // ---- Compute average absolute amplitude for this block ----
  long sumAbs = 0;

  for (int i = 0; i < samplesRead; i++) {
    int16_t s = sampleBuffer[i];
    if (s < 0) s = -s;
    sumAbs += s;
  }

  float avgLoudness = 0.0f;
  if (samplesRead > 0) {
    avgLoudness = (float)sumAbs / (float)samplesRead;
  }
  samplesRead = 0;    // Clear the read count

  // ---- Low-pass filter over time (simple 1st-order IIR) ----
  // filtered = filtered + alpha * (input - filtered)
  filteredLoudness = filteredLoudness + getFilterAlpha() * (avgLoudness - filteredLoudness);

  // ---- Track min/max over a fixed 5s window ----
  uint16_t loud_u16 = 0;
  if (filteredLoudness <= 0.0f) {
    loud_u16 = 0;
  } else if (filteredLoudness >= 65535.0f) {
    loud_u16 = 65535;
  } else {
    loud_u16 = (uint16_t)(filteredLoudness + 0.5f);
  }

  uint32_t now_ms = millis();
  if (vol_window_start_ms == 0) {
    vol_window_start_ms = now_ms;
  }
  if (loud_u16 < vol_window_min) vol_window_min = loud_u16;
  if (loud_u16 > vol_window_max) vol_window_max = loud_u16;

  uint8_t ledMode = getLedMode();
  if ((uint32_t)(now_ms - vol_window_start_ms) >= VOL_RANGE_WINDOW_MS) {
    if (vol_window_min == 0xFFFF) vol_window_min = 0;
    bleSetVolMinMax(vol_window_min, vol_window_max);

    if (ledMode == MODE_AEQ) {
      float targetNoiseFloor = (float)vol_window_min;
      float dynamicRange = (float)vol_window_max - (float)vol_window_min;
      if (dynamicRange < 1.0f) dynamicRange = 1.0f;
      float targetSensitivity = 100.0f / dynamicRange;

      float aeqAlpha = bleGetAeqFilterAlpha();
      if (aeqAlpha < 0.0f) aeqAlpha = 0.0f;
      if (aeqAlpha > 1.0f) aeqAlpha = 1.0f;

      autoNoiseFloorF += aeqAlpha * (targetNoiseFloor - autoNoiseFloorF);
      autoSensitivityF += aeqAlpha * (targetSensitivity - autoSensitivityF);
    }

    // Restart window
    vol_window_start_ms = now_ms;
    vol_window_min = 0xFFFF;
    vol_window_max = 0;
  }

  float effectiveNoiseFloor = bleGetNoiseFloor();
  float effectiveSensitivity = sensitivityF;
  if (ledMode == MODE_AEQ) {
    effectiveNoiseFloor = autoNoiseFloorF;
    effectiveSensitivity = autoSensitivityF;
  } else {
    autoNoiseFloorF = effectiveNoiseFloor;
    autoSensitivityF = effectiveSensitivity;
  }

  // ---- Apply noise floor ----
  float effectiveLoudness = filteredLoudness - effectiveNoiseFloor;
  if (effectiveLoudness < 0.0f) {
    effectiveLoudness = 0.0f;
  }

  // ---- Map loudness to LED brightness ----
  float ledValueF = effectiveLoudness * effectiveSensitivity * ((float)brightness)/100.0f;

  if (ledValueF > (float)pwmMax)
    ledValueF = (float)pwmMax;

  int ledValue = (int)ledValueF;
  
  if (ledValue > brightness)
    ledValue = brightness;

  switch(ledMode)
  {
    case MODE_OFF: ledValue = 0; break;
    case MODE_CONSTANT: ledValue = brightness; break;
    case MODE_POWEROFF: ledValue = 0; break;
    default:
      break;
  }

  unsigned long t = millis();
  if(t > 500UL){
    analogWrite(LED_CHANNEL, ledValue);
  }
  //analogWrite(LED_BUILTIN, pwmMax-ledValue);
  
#ifdef SERIAL_OUTPUT
  // Plot the filtered loudness (or effectiveLoudness) in Serial Plotter
  /*Serial.print(effectiveLoudness);
  Serial.print(", ");
  Serial.print(filteredLoudness);
  Serial.print(", ");
  Serial.print(ledValue);      
  Serial.print(", ");
  Serial.print(sensitivityF);      
  Serial.print(", ");
  Serial.print(((float)brightness)/100.0f);
  Serial.print(", ");
  Serial.print(getBatteryVoltage());
  Serial.print("\n");*/
#endif
}

void shutdownISR()
{
  shutdownIrqSeen = true;
}

void setup() {
  // If low-battery reset flag was set, go directly to SYSTEMOFF.
  if (NRF_POWER->GPREGRET == 0xA7)
  {
    NRF_POWER->GPREGRET = 0;

    flash.begin();
    Bluefruit.begin();

    if (!deepPowerDown(flash, flashTransport)) {
      pinMode(LED_BUILTIN, OUTPUT);
      digitalWrite(LED_BUILTIN, LOW);
      while (1) {
        yield();
      }
    }
    
    flash.end();
    sd_power_system_off();
  }
  
  batteryInit();

  Serial.begin(115200);
  randomSeed(NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1] ^ micros());

  analogWriteResolution(PWM_RES_BITS);
  pinMode(LED_CHANNEL, OUTPUT);
  analogWrite(LED_CHANNEL, 0);
  pinMode(LED_BUILTIN, OUTPUT);

#ifdef USE_PDM
  // Configure the data receive callback
  PDM.onReceive(onPDMdata);

  // Optionally set the gain
  //PDM.setGain(25);

  if (!PDM.begin(channels, frequency)) {
    Serial.println("Failed to start PDM!");
    while (1);
  }
#endif

  NRF_POWER->DCDCEN = 1;  //5.4mA-5.04mA
  //NRF_POWER->DCDCEN = 0;    //8.4mA

  /*pinMode(7, INPUT_PULLDOWN);
  pinMode(9, OUTPUT);
  digitalWrite(9, HIGH);
  pinMode(10, INPUT_PULLUP);*/
  //attachInterrupt(digitalPinToInterrupt(7), shutdownISR, RISING);

  pinMode(D2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(D2), shutdownISR, FALLING);

  Serial.println("PDM audio - started");

  /*if(chargeCurrentSelection == charge_current_100)
  {
    digitalWrite(P0_13, LOW);
    pinMode (P0_13, OUTPUT);
  }
  else
  {
    pinMode (P0_13, INPUT);
  }*/

  //pinMode(PIN_BAT_EN, INPUT);

#ifdef USE_BLE
  bleInit(brightness);     // your existing global brightness
#endif

#if ENABLE_NFC
  // NFC Type 2 Tag setup
  int nfc_err = nfc_t2t_setup(nfc_cb, nullptr);
  if (nfc_rc_ok(nfc_err)) {
    // nfc_t2t_payload_set expects an NDEF payload buffer in RAM.
    static uint8_t ndef_msg[260];
    size_t ndef_len = 0;

    if (buildNdefUriRecord(NFC_URI_TEXT, NFC_URI_PREFIX, ndef_msg, sizeof(ndef_msg), &ndef_len)) {
      nfc_err = nfc_t2t_payload_set(ndef_msg, ndef_len);
      if (nfc_rc_ok(nfc_err)) {
        nfc_err = nfc_t2t_emulation_start();
      }
    } else {
      Serial.println("NFC URI build failed");
    }
  }
#endif
}

void loop() 
{
  if (Serial.available() > 0)
  {
    char c = Serial.read();
    if(c == 'o')
      lowPowerTest = true;
  }

  batteryUpdate(bleGetVbatThres());
  bool low_power_condition = ((batteryIsLow() == true) && (vbusConnected() == false)) ||
                             lowPowerTest ||
                             ((getBatteryVoltage() < 3.73f) && (getBatteryVoltage() > 0.2));

  if (shutdownIrqSeen) {
    Serial.print("ISR");
    shutdownIrqSeen = false;
  }

#if ENABLE_NFC
  if (nfcEffectTrigger) {
    nfcEffectTrigger = false;
    nfcEffectActive = true;
    nfcEffectStartMs = millis();
  }
#endif

  if(lowPowerTest)
  {
    pinMode(LED_CHANNEL, INPUT_PULLDOWN);  //analogWrite(LED_CHANNEL, 0); 

    NRF_POWER->GPREGRET  = 0xA7;
    __DSB(); __ISB();
    NVIC_SystemReset();
  }

  if (low_power_condition) {
    // Immediately stop LEDs
    analogWrite(LED_CHANNEL, 0);
    //analogWrite(LED_BUILTIN, pwmMax);
    digitalWrite(LED_BUILTIN, HIGH);   // off (active-low)

    delay(1000);
    Serial.write("Step 1");

    if(((getBatteryVoltage() < 3.73f) && (getBatteryVoltage() > 0.2)) || lowPowerTest)    //if battery is very low, completly turn off
    {
      delay(1000);
      Serial.write("Step 2");

      if(lowBattery == false)
      {
        lowBattery = true;
                
        pinMode(LED_CHANNEL, INPUT_PULLDOWN);  //analogWrite(LED_CHANNEL, 0); 
        // pinMode(LED_BUILTIN, INPUT);   // off
        // #ifdef USE_PDM
        // PDM.end();
        // #endif
        // ble_sleep();

        delay(1000);
        Serial.write("Step 3");

        NRF_POWER->GPREGRET  = 0xA7;
        __DSB(); __ISB();
        NVIC_SystemReset();
      }      
    }
    else    //Otherwise we keep running without LEDs
    {
      Serial.println("Vbat low");
      delay(1000);
    }
  }
  if (!low_power_condition)
  {
#if ENABLE_NFC
    if (nfcEffectActive) {
      fireflyActive = false;
      updateNfcEffect();
    } else
#endif
    {
      if (getLedMode() == MODE_FIREFLY) {
        updateFirefly(millis());
      } else {
        fireflyActive = false;
        if (samplesRead) {
          // Wait for samples to be read
          updateLEDs();
        }
      }
    }

#ifdef USE_BLE
    uint32_t nowMs = millis();

    if ((uint32_t)(nowMs - lastBleCtrlUpdateMs) >= BLE_CTRL_UPDATE_PERIOD_MS) {
      lastBleCtrlUpdateMs = nowMs;
      bleUpdate();

      // The ends of the pot are only powered for its brief, periodic read.
      updatePotentiometer(nowMs);
      brightness = bleGetBrightness();
      sensitivityF = bleGetSensitivity();
    }

    if ((uint32_t)(nowMs - lastBleVbatUpdateMs) >= BLE_VBAT_UPDATE_PERIOD_MS) {
      lastBleVbatUpdateMs = nowMs;
      bleSetVBat_mV(getBatteryVoltage_mV());
    }
#endif
  }
  delay(2);
}

/**
 * Callback function to process the data from the PDM microphone.
 * NOTE: This callback is executed as part of an ISR.
 * Therefore using `Serial` to print messages inside this function isn't supported.
 */
#ifdef USE_PDM
void onPDMdata() {
  // Query the number of available bytes
  int bytesAvailable = PDM.available();
  
  // Read into the sample buffer
  PDM.read(sampleBuffer, bytesAvailable);
  
  // 16-bit, 2 bytes per sample
  samplesRead = bytesAvailable / 2;
}
#endif
