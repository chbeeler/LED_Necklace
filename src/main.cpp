#include <Arduino.h>
//#include "board_pins.h"

#include "battery.h"
#include "ble_control.h"
#include "audio_engine.h"
#include "button_controller.h"
#include "firefly_effect.h"
#include "led_modes.h"
#include "led_output.h"
#include "nfc_service.h"

#include <Adafruit_SPIFlash.h>
#include <LSM6DS3.h>
#include <bluefruit.h>
//#include <nrf52840.h>

#define USE_BLE

#define BUTTON_POWER_OFF_DELAY_MS 5000UL
#define BUTTON_WAKE_MAGIC       0xB2
#define SYSTEM_OFF_MAGIC        0xA7

static const uint16_t BRIGHTNESS_LEVELS[] = {
  0, 5, 10, 20, 30, 50, 70, 100, 150, 250, 400
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
uint32_t lastBleCtrlUpdateMs = 0;
uint32_t lastBleVbatUpdateMs = 0;

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

static void changeSecondarySetting(bool increase)
{
  const uint8_t mode = getLedMode();
  if (mode == MODE_AEQ) {
    const uint8_t current = bleGetAeqIdlePercent();
    const uint8_t next = increase ? (current < 10 ? current + 1 : 10)
                                  : (current > 0 ? current - 1 : 0);
    if (next == current) return;
    bleSetAeqIdlePercentFromButtons(next);
    Serial.print("AEQ idle brightness: ");
    Serial.print(next);
    Serial.println('%');
    return;
  }

  if (mode == MODE_CONSTANT) {
    const uint16_t current = bleGetBrightness();
    const uint16_t next = increase ? (current < 4095 ? current + 1 : 4095)
                                   : (current > 0 ? current - 1 : 0);
    if (next == current) return;
    bleSetBrightnessFromButtons(next);
    brightness = next;
    Serial.print("Fine brightness: ");
    Serial.println(next);
    return;
  }

  Serial.println("Secondary adjustment ignored in this LED mode");
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
  bleSetModeFromButtons(nextMode);
  Serial.println(nextMode == MODE_AEQ ? "LED mode: AEQ" :
                                       "LED mode: constant");
}

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
  NfcService::prepare();
  bleInit(brightness);     // your existing global brightness
#endif
  NfcService::begin();
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

  NfcService::update(millis());

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
    const bool effectOwnsLeds =
        activeLedMode == MODE_FIREFLY || NfcService::effectActive();

    const AudioSettings audioSettings = {
      activeLedMode,
      (uint16_t)brightness,
      sensitivityF,
      getFilterAlpha(),
      bleGetNoiseFloor(),
      bleGetAeqReleaseMs(),
      bleGetAeqIdlePercent(),
    };
    const AudioResult audioResult =
        AudioEngine::update(audioSettings, !effectOwnsLeds, millis());
    if (audioResult.volumeRangeReady) {
      bleSetVolMinMax(audioResult.volumeMin, audioResult.volumeMax);
    }
    if (audioResult.telemetryReady) {
      bleSetAudioTelemetry(audioResult.telemetry);
    }

    if (NfcService::effectActive()) {
      fireflyEffect.reset();
      uint16_t nfcBrightness = 0;
      if (NfcService::renderEffect(millis(), nfcBrightness)) {
        LedOutput::write(nfcBrightness);
      }
    } else {
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
    switch (ButtonController::update(millis())) {
      case ButtonAction::BrightnessUp:
        changeBrightness(true);
        break;
      case ButtonAction::BrightnessDown:
        changeBrightness(false);
        break;
      case ButtonAction::SecondaryUp:
        changeSecondarySetting(true);
        break;
      case ButtonAction::SecondaryDown:
        changeSecondarySetting(false);
        break;
      case ButtonAction::ToggleAeqConstant:
        toggleAeqConstantMode();
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
