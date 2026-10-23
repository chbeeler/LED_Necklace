#include <Arduino.h>
//#include "board_pins.h"

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
#include "audio_engine.h"
#include "button_controller.h"
#include "firefly_effect.h"
#include "led_modes.h"
#include "led_output.h"

#include <Adafruit_SPIFlash.h>
#include <LSM6DS3.h>
#include <bluefruit.h>
#include <math.h>
#include <string.h>
//#include <nrf52840.h>

#define USE_BLE

#define BUTTON_POWER_OFF_DELAY_MS 5000UL
#define BUTTON_WAKE_MAGIC       0xB2
#define SYSTEM_OFF_MAGIC        0xA7

static const uint16_t BRIGHTNESS_LEVELS[] = {
  5, 10, 20, 30, 50, 70, 100, 150, 250, 400
};
static const size_t BRIGHTNESS_LEVEL_COUNT =
    sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);

extern BLEUart bleuart;   // global

typedef enum {charge_current_50, charge_current_100} charge_current_setting_t;
charge_current_setting_t chargeCurrentSelection = charge_current_50;

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

static uint8_t previousOnMode = MODE_AEQ;

#if ENABLE_NFC
static const uint32_t NFC_EFFECT_DURATION_MS = 4000UL;
static const float NFC_EFFECT_FREQUENCY_HZ = 0.8f;
static const int NFC_EFFECT_MIN = 5;
static const int NFC_EFFECT_MAX = 250;
#endif
static const uint32_t BLE_CTRL_UPDATE_PERIOD_MS = 20UL;
static const uint32_t BLE_VBAT_UPDATE_PERIOD_MS = 500UL;

static FireflyEffect fireflyEffect;

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
    LedOutput::write(0);
    Serial.println("LED output: off");
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
  } else if (ledValue > (int)LedOutput::PWM_MAX) {
    ledValue = LedOutput::PWM_MAX;
  }

  LedOutput::write(ledValue);
}

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

static void setDiagnosticLed(bool on)
{
  // The XIAO's onboard LED is active-low.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

static void blinkDiagnosticLed(uint8_t count, uint16_t onMs = 120,
                               uint16_t offMs = 120)
{
  for (uint8_t i = 0; i < count; ++i) {
    setDiagnosticLed(true);
    delay(onMs);
    setDiagnosticLed(false);
    delay(offMs);
  }
}

static bool waitForButtonPowerOnGesture()
{
  uint32_t bothPressedSinceMs = 0;
  uint32_t bothReleasedSinceMs = 0;

  Serial.println("Button wake: hold both buttons for 1.5 seconds to power on");
  while (true) {
    const bool plusPressed = ButtonController::plusPressedRaw();
    const bool minusPressed = ButtonController::minusPressedRaw();
    const uint32_t nowMs = millis();

    // Wake-gate diagnostics on the onboard LED:
    //   one button = blinking, both buttons = solid, neither = off.
    if (plusPressed && minusPressed) {
      setDiagnosticLed(true);
    } else if (plusPressed || minusPressed) {
      setDiagnosticLed(((nowMs / 200UL) & 1UL) != 0);
    } else {
      setDiagnosticLed(false);
    }

    if (plusPressed && minusPressed) {
      bothReleasedSinceMs = 0;
      if (bothPressedSinceMs == 0) {
        bothPressedSinceMs = nowMs;
      } else if ((uint32_t)(nowMs - bothPressedSinceMs) >=
                 ButtonController::LONG_PRESS_MS) {
        Serial.println("Both buttons held: powering on");
        setDiagnosticLed(false);
        blinkDiagnosticLed(3, 80, 80);
        return true;
      }
    } else {
      bothPressedSinceMs = 0;
      if (!plusPressed && !minusPressed) {
        if (bothReleasedSinceMs == 0) {
          bothReleasedSinceMs = nowMs;
        } else if ((uint32_t)(nowMs - bothReleasedSinceMs) >=
                   ButtonController::RELEASE_GUARD_MS) {
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
  LedOutput::write(0);
  Serial.println("LEDs off; system shutdown in 5 seconds");
  delay(BUTTON_POWER_OFF_DELAY_MS);
  ButtonController::waitForRelease();

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

void shutdownISR()
{
  shutdownIrqSeen = true;
}

void setup() {
  const uint32_t resetReason = NRF_POWER->RESETREAS;
  const uint8_t retainedMarker = (uint8_t)NRF_POWER->GPREGRET;
  NRF_POWER->RESETREAS = resetReason;

  // One early flash means setup really was reached after SYSTEMOFF with the
  // expected marker. Four flashes mean SYSTEMOFF reset occurred but the
  // marker was missing or changed (for example by an earlier boot stage).
  if ((resetReason & POWER_RESETREAS_OFF_Msk) != 0) {
    blinkDiagnosticLed(retainedMarker == BUTTON_WAKE_MAGIC ? 1 : 4);
  }

  // A GPIO event can wake SYSTEMOFF on either button. Do not perform normal
  // startup until both buttons have been held for the intended power gesture.
  if (retainedMarker == BUTTON_WAKE_MAGIC)
  {
    Serial.begin(115200);
    Serial.print("SYSTEMOFF wake; RESETREAS=0x");
    Serial.print(resetReason, HEX);
    Serial.print(", GPREGRET=0x");
    Serial.println(retainedMarker, HEX);

    if (vbusConnected()) {
      // USB is an independent nRF52840 SYSTEMOFF wake source and remains the
      // recovery/development path: connecting it always permits normal boot.
      Serial.println("USB wake: bypassing button power-on gate");
      blinkDiagnosticLed(2, 80, 80);
    } else {
      ButtonController::begin();
      if (!waitForButtonPowerOnGesture()) {
        setDiagnosticLed(false);
        resetIntoSystemOffPath();
      }
      ButtonController::waitForRelease();
    }

    NRF_POWER->GPREGRET = 0;
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
      ButtonController::begin();
      ButtonController::waitForRelease();
      ButtonController::configureWakeSources();
      // Bluefruit has enabled the SoftDevice, so use its API for the retained
      // marker instead of writing the protected POWER register directly.
      sd_power_gpregret_clr(0, 0xFF);
      sd_power_gpregret_set(0, BUTTON_WAKE_MAGIC);
      __DSB();
      __ISB();
      sd_power_system_off();
    }
  }
  
  batteryInit();

  Serial.begin(115200);
  ButtonController::begin();
  randomSeed(NRF_FICR->DEVICEID[0] ^ NRF_FICR->DEVICEID[1] ^ micros());

  LedOutput::begin();
  pinMode(LED_BUILTIN, OUTPUT);

  if (!AudioEngine::begin()) {
    Serial.println("Failed to start PDM!");
    while (1);
  }

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
    LedOutput::setPinMode(INPUT_PULLDOWN);

    resetIntoSystemOffPath();
  }

  if (low_power_condition) {
    // Immediately stop LEDs
    LedOutput::write(0);
    // analogWrite(LED_BUILTIN, LedOutput::PWM_MAX);
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
                
        LedOutput::setPinMode(INPUT_PULLDOWN);
        // pinMode(LED_BUILTIN, INPUT);   // off
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
    const uint8_t activeLedMode = getLedMode();
    bool effectOwnsLeds = activeLedMode == MODE_FIREFLY;
#if ENABLE_NFC
    effectOwnsLeds = effectOwnsLeds || nfcEffectActive;
#endif

    const AudioSettings audioSettings = {
      activeLedMode,
      (uint16_t)brightness,
      sensitivityF,
      getFilterAlpha(),
      bleGetNoiseFloor(),
    };
    const AudioResult audioResult =
        AudioEngine::update(audioSettings, !effectOwnsLeds, millis());
    if (audioResult.volumeRangeReady) {
      bleSetVolMinMax(audioResult.volumeMin, audioResult.volumeMax);
    }

#if ENABLE_NFC
    if (nfcEffectActive) {
      fireflyEffect.reset();
      updateNfcEffect();
    } else
#endif
    {
      if (activeLedMode == MODE_FIREFLY) {
        LedOutput::write(fireflyEffect.update(millis(), brightness));
      } else {
        fireflyEffect.reset();
        if (audioResult.ledValueReady) {
          LedOutput::write(audioResult.ledValue);
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
    switch (ButtonController::update(millis())) {
      case ButtonAction::BrightnessUp:
        changeBrightness(true);
        break;
      case ButtonAction::BrightnessDown:
        changeBrightness(false);
        break;
      case ButtonAction::ToggleAeqConstant:
        toggleAeqConstantMode();
        break;
      case ButtonAction::ToggleLedOutput:
        toggleLedOutput();
        break;
      case ButtonAction::RequestSystemOff:
        requestButtonSystemOff();
        break;
      case ButtonAction::None:
        break;
    }
  }
  delay(2);
}
