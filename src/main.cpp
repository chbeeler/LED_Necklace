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
#include "adaptive_brightness.h"

#include <Adafruit_SPIFlash.h>
#include <LSM6DS3.h>
#include <bluefruit.h>
#include <math.h>
#include <string.h>
//#include <nrf52840.h>

#define SERIAL_OUTPUT
#define USE_BLE

// Each button is wired between two GPIO pins.  Drive one side HIGH and use a
// pull-down on the other side, so a closed button reads HIGH.
#define BUTTON_1_DRIVE_PIN      D7
#define BUTTON_1_SENSE_PIN      D9
#define BUTTON_2_DRIVE_PIN      D4
#define BUTTON_2_SENSE_PIN      D6
#define BUTTON_DEBOUNCE_MS      25UL
#define BUTTON_LONG_PRESS_MS    1500UL
#define BUTTON_RELEASE_GUARD_MS 100UL
#define BUTTON_POWER_OFF_DELAY_MS 5000UL
#define BUTTON_WAKE_MAGIC       0xB2
#define SYSTEM_OFF_MAGIC        0xA7

static const uint16_t BRIGHTNESS_LEVELS[] = {
  5, 10, 20, 30, 50, 70, 100, 150, 250, 400
};
static const size_t BRIGHTNESS_LEVEL_COUNT =
    sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);

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

#define LED_CHANNEL          D3  // Existing necklace PCB connection.
#define LED_CHANNEL_DUPLICATE D0 // Easier-to-solder connection for new PCBs.

static void writeLedPwm(uint32_t value)
{
  // Both pins are channels of the same hardware PWM peripheral, so they carry
  // the same waveform while retaining compatibility with the original pinout.
  analogWrite(LED_CHANNEL, value);
  analogWrite(LED_CHANNEL_DUPLICATE, value);
}

static void setLedPinMode(uint8_t mode)
{
  pinMode(LED_CHANNEL, mode);
  pinMode(LED_CHANNEL_DUPLICATE, mode);
}

typedef enum {charge_current_50, charge_current_100} charge_current_setting_t;
charge_current_setting_t chargeCurrentSelection = charge_current_50;

// default number of output channels
static const char channels = 1;

// default PCM output frequency
static const int frequency = 16000;

// The PDM callback fills one buffer while the main loop processes the other.
// The library currently supplies 512-byte blocks (256 mono int16 samples), and
// the larger capacity leaves room if its internal block size is changed later.
static const uint16_t AUDIO_BUFFER_SAMPLES = 512;
static int16_t audioBuffers[2][AUDIO_BUFFER_SAMPLES];
static volatile int8_t readyAudioBuffer = -1;
static volatile uint16_t readyAudioSamples = 0;
static volatile uint8_t pdmWriteBuffer = 0;
static volatile uint32_t droppedAudioBlocks = 0;

// ---- Beat / loudness detection variables ----

// Low-pass filtered loudness value
float filteredLoudness = 0.0f;
AdaptiveBrightness adaptiveBrightness;
static uint8_t previousAudioMode = 0xFF;

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
bool lowPowerTest = false;
volatile bool shutdownIrqSeen = false;
#if ENABLE_NFC
volatile bool nfcEffectTrigger = false;
bool nfcEffectActive = false;
uint32_t nfcEffectStartMs = 0;
#endif
uint32_t lastBleCtrlUpdateMs = 0;
uint32_t lastBleVbatUpdateMs = 0;

struct ButtonState {
  bool rawPressed;
  bool stablePressed;
  uint32_t rawChangedMs;
  uint32_t pressedMs;
};

static ButtonState button1State = {false, false, 0, 0};
static ButtonState button2State = {false, false, 0, 0};
static bool plusLongHandled = false;
static bool minusLongHandled = false;
static bool chordActive = false;
static bool chordHandled = false;
static uint32_t chordStartedMs = 0;
static bool buttonSystemOffRequested = false;
static uint8_t previousOnMode = MODE_AEQ;

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
  writeLedPwm(ledValue);
}

static void setupButtons()
{
  // Set the output latch before enabling output mode to avoid a LOW glitch.
  digitalWrite(BUTTON_1_DRIVE_PIN, HIGH);
  pinMode(BUTTON_1_DRIVE_PIN, OUTPUT);
  pinMode(BUTTON_1_SENSE_PIN, INPUT_PULLDOWN);

  digitalWrite(BUTTON_2_DRIVE_PIN, HIGH);
  pinMode(BUTTON_2_DRIVE_PIN, OUTPUT);
  pinMode(BUTTON_2_SENSE_PIN, INPUT_PULLDOWN);
}

static void updateButton(ButtonState &state,
                         bool rawPressed,
                         uint32_t nowMs,
                         const char *name)
{
  if (rawPressed != state.rawPressed) {
    state.rawPressed = rawPressed;
    state.rawChangedMs = nowMs;
  }

  if (state.stablePressed != state.rawPressed &&
      (uint32_t)(nowMs - state.rawChangedMs) >= BUTTON_DEBOUNCE_MS) {
    state.stablePressed = state.rawPressed;
    if (state.stablePressed) {
      state.pressedMs = nowMs;
      Serial.print(name);
      Serial.println(" pressed");
    }
  }
}

static void changeBrightness(bool increase)
{
  const uint16_t current = bleGetBrightness();
  uint16_t next = current;

  if (increase) {
    for (size_t i = 0; i < BRIGHTNESS_LEVEL_COUNT; ++i) {
      if (BRIGHTNESS_LEVELS[i] > current) {
        next = BRIGHTNESS_LEVELS[i];
        break;
      }
    }
  } else {
    for (size_t i = BRIGHTNESS_LEVEL_COUNT; i > 0; --i) {
      if (BRIGHTNESS_LEVELS[i - 1] < current) {
        next = BRIGHTNESS_LEVELS[i - 1];
        break;
      }
    }
  }

  if (next == current) return;
  bleSetBrightnessFromButtons(next);
  brightness = next;
  Serial.print("Brightness: ");
  Serial.println(next);
}

static void toggleAeqConstantMode()
{
  const uint8_t currentMode = getLedMode();
  if (currentMode == MODE_OFF) {
    Serial.println("Plus long press ignored while LEDs are off");
    return;
  }

  const uint8_t nextMode =
      currentMode == MODE_AEQ ? MODE_CONSTANT : MODE_AEQ;
  previousOnMode = nextMode;
  bleSetModeFromButtons(nextMode);
  Serial.println(nextMode == MODE_AEQ ? "LED mode: AEQ" :
                                       "LED mode: constant");
}

static void toggleLedOutput()
{
  const uint8_t currentMode = getLedMode();
  if (currentMode == MODE_OFF) {
    if (previousOnMode == MODE_OFF || previousOnMode == MODE_POWEROFF) {
      previousOnMode = MODE_AEQ;
    }
    bleSetModeFromButtons(previousOnMode);
    Serial.println("LED output: on");
  } else {
    previousOnMode = currentMode;
    bleSetModeFromButtons(MODE_OFF);
    writeLedPwm(0);
    Serial.println("LED output: off");
  }
}

static void updateButtons(uint32_t nowMs)
{
  const bool plusWasPressed = button1State.stablePressed;
  const bool minusWasPressed = button2State.stablePressed;

  updateButton(button1State,
               digitalRead(BUTTON_1_SENSE_PIN) == HIGH,
               nowMs,
               "Plus (D7-D9)");
  updateButton(button2State,
               digitalRead(BUTTON_2_SENSE_PIN) == HIGH,
               nowMs,
               "Minus (D4-D6)");

  const bool plusPressed = button1State.stablePressed;
  const bool minusPressed = button2State.stablePressed;

  if (!plusWasPressed && plusPressed) plusLongHandled = false;
  if (!minusWasPressed && minusPressed) minusLongHandled = false;

  if (plusPressed && minusPressed) {
    if (!chordActive) {
      chordActive = true;
      chordHandled = false;
      chordStartedMs = nowMs;
      // A chord always takes priority over both individual gestures.
      plusLongHandled = true;
      minusLongHandled = true;
    }

    if (!chordHandled &&
        (uint32_t)(nowMs - chordStartedMs) >= BUTTON_LONG_PRESS_MS) {
      chordHandled = true;
      buttonSystemOffRequested = true;
      Serial.println("Both buttons held: system off requested");
    }
    return;
  }

  // Once a chord starts, suppress all individual actions until both buttons
  // have been released.
  if (chordActive) {
    if (!plusPressed && !minusPressed) {
      chordActive = false;
      chordHandled = false;
      plusLongHandled = false;
      minusLongHandled = false;
    }
    return;
  }

  if (plusPressed && !plusLongHandled &&
      (uint32_t)(nowMs - button1State.pressedMs) >= BUTTON_LONG_PRESS_MS) {
    plusLongHandled = true;
    toggleAeqConstantMode();
  }

  if (minusPressed && !minusLongHandled &&
      (uint32_t)(nowMs - button2State.pressedMs) >= BUTTON_LONG_PRESS_MS) {
    minusLongHandled = true;
    toggleLedOutput();
  }

  // Short actions happen on release, so a long press never also changes the
  // brightness preset.
  if (plusWasPressed && !plusPressed) {
    if (!plusLongHandled) changeBrightness(true);
    plusLongHandled = false;
  }

  if (minusWasPressed && !minusPressed) {
    if (!minusLongHandled) changeBrightness(false);
    minusLongHandled = false;
  }
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

  writeLedPwm(ledValue);
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

static void waitForButtonsReleased()
{
  uint32_t releasedSinceMs = 0;
  while (true) {
    const bool eitherPressed =
        digitalRead(BUTTON_1_SENSE_PIN) == HIGH ||
        digitalRead(BUTTON_2_SENSE_PIN) == HIGH;

    if (eitherPressed) {
      releasedSinceMs = 0;
    } else if (releasedSinceMs == 0) {
      releasedSinceMs = millis();
    } else if ((uint32_t)(millis() - releasedSinceMs) >=
               BUTTON_RELEASE_GUARD_MS) {
      return;
    }
    delay(5);
  }
}

static void configureButtonWakeSources()
{
  // Keep one side of each button driven HIGH and let either sense pin wake the
  // nRF52 from SYSTEMOFF. The retained marker in GPREGRET makes setup require
  // the full two-button gesture before completing startup.
  digitalWrite(BUTTON_1_DRIVE_PIN, HIGH);
  pinMode(BUTTON_1_DRIVE_PIN, OUTPUT);
  digitalWrite(BUTTON_2_DRIVE_PIN, HIGH);
  pinMode(BUTTON_2_DRIVE_PIN, OUTPUT);
  pinMode(BUTTON_1_SENSE_PIN, INPUT_PULLDOWN_SENSE);
  pinMode(BUTTON_2_SENSE_PIN, INPUT_PULLDOWN_SENSE);

  // Clear any GPIO event latched while configuring SENSE. Both buttons have
  // already been verified released, so a later HIGH is a genuine wake event.
  NRF_P0->LATCH = 0xFFFFFFFFUL;
  NRF_P1->LATCH = 0xFFFFFFFFUL;
}

static bool waitForButtonPowerOnGesture()
{
  uint32_t bothPressedSinceMs = 0;
  uint32_t bothReleasedSinceMs = 0;

  Serial.println("Button wake: hold both buttons for 1.5 seconds to power on");
  while (true) {
    const bool plusPressed = digitalRead(BUTTON_1_SENSE_PIN) == HIGH;
    const bool minusPressed = digitalRead(BUTTON_2_SENSE_PIN) == HIGH;
    const uint32_t nowMs = millis();

    if (plusPressed && minusPressed) {
      bothReleasedSinceMs = 0;
      if (bothPressedSinceMs == 0) {
        bothPressedSinceMs = nowMs;
      } else if ((uint32_t)(nowMs - bothPressedSinceMs) >=
                 BUTTON_LONG_PRESS_MS) {
        Serial.println("Both buttons held: powering on");
        return true;
      }
    } else {
      bothPressedSinceMs = 0;
      if (!plusPressed && !minusPressed) {
        if (bothReleasedSinceMs == 0) {
          bothReleasedSinceMs = nowMs;
        } else if ((uint32_t)(nowMs - bothReleasedSinceMs) >=
                   BUTTON_RELEASE_GUARD_MS) {
          return false;
        }
      } else {
        bothReleasedSinceMs = 0;
      }
    }
    delay(5);
  }
}

static void resetIntoSystemOffPath()
{
  NRF_POWER->GPREGRET = SYSTEM_OFF_MAGIC;
  __DSB();
  __ISB();
  NVIC_SystemReset();
}

static void requestButtonSystemOff()
{
  // Make the shutdown immediately visible, then deliberately leave the MCU
  // awake for five seconds so the transition is easy to observe and debug.
  writeLedPwm(0);
  Serial.println("LEDs off; system shutdown in 5 seconds");
  delay(BUTTON_POWER_OFF_DELAY_MS);
  waitForButtonsReleased();

  // Use exactly the same shutdown request consumed by the BLE/app power-off
  // command. The next loop iteration records GPREGRET and resets; setup then
  // enables Bluefruit before calling sd_power_system_off().
  lowPowerTest = true;
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

static bool takeAudioBlock(uint8_t& bufferIndex, uint16_t& sampleCount)
{
  noInterrupts();
  if (readyAudioBuffer < 0) {
    interrupts();
    return false;
  }

  bufferIndex = (uint8_t)readyAudioBuffer;
  sampleCount = readyAudioSamples;
  readyAudioBuffer = -1;
  readyAudioSamples = 0;
  interrupts();
  return true;
}

void updateLEDs(const int16_t* samples, uint16_t sampleCount)
{
  // ---- Compute average absolute amplitude for this block ----
  long sumAbs = 0;

  for (uint16_t i = 0; i < sampleCount; i++) {
    int32_t s = samples[i];
    if (s < 0) s = -s;
    sumAbs += s;
  }

  float avgLoudness = 0.0f;
  if (sampleCount > 0) {
    avgLoudness = (float)sumAbs / (float)sampleCount;
  }

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

    // Restart window
    vol_window_start_ms = now_ms;
    vol_window_min = 0xFFFF;
    vol_window_max = 0;
  }

  float ledValueF = 0.0f;
  if (ledMode == MODE_AEQ) {
    // The adaptive processor normalizes signed PDM samples to -1..1, follows
    // the ambient floor and typical peaks, and adds transient emphasis.
    const float adaptiveOutput = adaptiveBrightness.process(
        samples, sampleCount, (uint32_t)frequency, sensitivityF);
    const uint16_t brightnessLimit = brightness < pwmMax ? brightness : pwmMax;
    ledValueF = adaptiveOutput * (float)brightnessLimit;
  } else {
    // Preserve the established MODE_EQ response and its BLE tuning controls.
    float effectiveLoudness = filteredLoudness - bleGetNoiseFloor();
    if (effectiveLoudness < 0.0f) effectiveLoudness = 0.0f;
    ledValueF = effectiveLoudness * sensitivityF * ((float)brightness) / 100.0f;
  }

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
    writeLedPwm(ledValue);
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
  // A GPIO event can wake SYSTEMOFF on either button. Do not perform normal
  // startup until both buttons have been held for the intended power gesture.
  if (NRF_POWER->GPREGRET == BUTTON_WAKE_MAGIC)
  {
    Serial.begin(115200);
    setupButtons();

    if (!waitForButtonPowerOnGesture()) {
      resetIntoSystemOffPath();
    }

    NRF_POWER->GPREGRET = 0;
    waitForButtonsReleased();
  }

  // If a low-battery reset was requested, enter SYSTEMOFF only when USB is
  // still absent.  Connecting USB must always allow the device to boot, even
  // when no battery is installed or a previous boot set the reset marker.
  if (NRF_POWER->GPREGRET == SYSTEM_OFF_MAGIC)
  {
    NRF_POWER->GPREGRET = 0;

    if (!vbusConnected()) {
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
      setupButtons();
      waitForButtonsReleased();
      configureButtonWakeSources();
      NRF_POWER->GPREGRET = BUTTON_WAKE_MAGIC;
      __DSB();
      __ISB();
      sd_power_system_off();
    }
  }
  
  batteryInit();

  Serial.begin(115200);
  setupButtons();
  randomSeed(NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1] ^ micros());

  analogWriteResolution(PWM_RES_BITS);
  setLedPinMode(OUTPUT);
  writeLedPwm(0);
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
  bool usbPowered = vbusConnected();
  float batteryVoltage = getBatteryVoltage();
  bool batteryVoltageCritical = batteryVoltage < 3.73f && batteryVoltage > 0.2f;
  bool low_power_condition = lowPowerTest || (!usbPowered && (batteryIsLow() || batteryVoltageCritical));

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
    setLedPinMode(INPUT_PULLDOWN);

    resetIntoSystemOffPath();
  }

  if (low_power_condition) {
    // Immediately stop LEDs
    writeLedPwm(0);
    //analogWrite(LED_BUILTIN, pwmMax);
    digitalWrite(LED_BUILTIN, HIGH);   // off (active-low)

    delay(1000);
    Serial.write("Step 1");

    // Explicit test shutdown still works on USB. Automatic battery shutdown
    // is allowed only while USB power is absent.
    if ((!usbPowered && batteryVoltageCritical) || lowPowerTest)
    {
      delay(1000);
      Serial.write("Step 2");

      if(lowBattery == false)
      {
        lowBattery = true;
                
        setLedPinMode(INPUT_PULLDOWN);
        // pinMode(LED_BUILTIN, INPUT);   // off
        // #ifdef USE_PDM
        // PDM.end();
        // #endif
        // ble_sleep();

        delay(1000);
        Serial.write("Step 3");

        resetIntoSystemOffPath();
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
    uint8_t audioBufferIndex = 0;
    uint16_t audioSampleCount = 0;
    const bool audioBlockReady = takeAudioBlock(audioBufferIndex, audioSampleCount);
    const uint8_t activeLedMode = getLedMode();
    if (activeLedMode != previousAudioMode) {
      if (activeLedMode == MODE_AEQ) adaptiveBrightness.reset();
      previousAudioMode = activeLedMode;
    }

#if ENABLE_NFC
    if (nfcEffectActive) {
      fireflyActive = false;
      updateNfcEffect();
    } else
#endif
    {
      if (activeLedMode == MODE_FIREFLY) {
        updateFirefly(millis());
      } else {
        fireflyActive = false;
        if (audioBlockReady) {
          updateLEDs(audioBuffers[audioBufferIndex], audioSampleCount);
        }
      }
    }

#ifdef USE_BLE
    uint32_t nowMs = millis();

    if ((uint32_t)(nowMs - lastBleCtrlUpdateMs) >= BLE_CTRL_UPDATE_PERIOD_MS) {
      lastBleCtrlUpdateMs = nowMs;
      bleUpdate();

      brightness = bleGetBrightness();
      sensitivityF = bleGetSensitivity();
    }

    if ((uint32_t)(nowMs - lastBleVbatUpdateMs) >= BLE_VBAT_UPDATE_PERIOD_MS) {
      lastBleVbatUpdateMs = nowMs;
      bleSetVBat_mV(getBatteryVoltage_mV());
    }
#endif
    const uint8_t currentButtonMode = getLedMode();
    if (currentButtonMode != MODE_OFF &&
        currentButtonMode != MODE_POWEROFF) {
      previousOnMode = currentButtonMode;
    }
    updateButtons(millis());
    if (buttonSystemOffRequested) {
      requestButtonSystemOff();
    }
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
  if (bytesAvailable <= 0) return;

  const uint8_t destination = pdmWriteBuffer;
  const int bufferBytes = (int)sizeof(audioBuffers[destination]);
  if (bytesAvailable > bufferBytes) bytesAvailable = bufferBytes;

  const int bytesRead = PDM.read(audioBuffers[destination], (size_t)bytesAvailable);
  if (bytesRead <= 0) return;

  // Publish only complete, stable buffers. If the main loop still owns the
  // previous block, retain that block and count this new one as dropped.
  if (readyAudioBuffer < 0) {
    readyAudioSamples = (uint16_t)(bytesRead / (int)sizeof(int16_t));
    readyAudioBuffer = (int8_t)destination;
    pdmWriteBuffer = destination ^ 1U;
  } else {
    ++droppedAudioBlocks;
  }
}
#endif
