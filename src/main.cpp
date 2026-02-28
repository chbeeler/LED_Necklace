#include <Arduino.h>
//#include "board_pins.h"
#define USE_PDM
#ifdef USE_PDM
#include <PDM.h>
#endif

extern "C" {
#include "nfc_t2t_lib.h"
}

#include "battery.h"
#include "ble_control.h"

#include <Adafruit_SPIFlash.h>
#include <LSM6DS3.h>
#include <bluefruit.h>
//#include <nrf52840.h>

#define SERIAL_OUTPUT
#define USE_BLE

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

unsigned long lastBatSampleMs = 0;
bool lowBattery = false;

int brightness = 100;
float sensitivityF = 8.0f;    //Scale mic loudness into LED brightness (tune this)

bool lowPowerTest = false;

#ifdef USE_PDM
void onPDMdata();
#endif

static void nfc_cb(void *context, nfc_t2t_event_t event, const uint8_t *data, size_t data_length)
{
  (void)context; (void)data; (void)data_length;
  // You can log events if you want
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

  if ((uint32_t)(now_ms - vol_window_start_ms) >= VOL_RANGE_WINDOW_MS) {
    if (vol_window_min == 0xFFFF) vol_window_min = 0;
    bleSetVolMinMax(vol_window_min, vol_window_max);

    // Restart window
    vol_window_start_ms = now_ms;
    vol_window_min = 0xFFFF;
    vol_window_max = 0;
  }

  // ---- Apply noise floor ----
  float effectiveLoudness = filteredLoudness - bleGetNoiseFloor();
  if (effectiveLoudness < 0.0f) {
    effectiveLoudness = 0.0f;
  }

  // ---- Map loudness to LED brightness ----
  float ledValueF = effectiveLoudness * sensitivityF * ((float)brightness)/100.0f;

  if (ledValueF > (float)pwmMax)
    ledValueF = (float)pwmMax;

  int ledValue = (int)ledValueF;
  
  if (ledValue > brightness)
    ledValue = brightness;

  switch(getLedMode())
  {
    case 0: ledValue = 0; break;
    case 2: ledValue = brightness; break;
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
  Serial.println("ISR");
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
  /*while (!Serial) {
    // wait for serial
  }*/

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

  // NFC Type 2 Tag setup
  int nfc_err = nfc_t2t_setup(nfc_cb, nullptr);
  if (nfc_err != 0) {
    Serial.print("NFC setup failed: ");
    Serial.println(nfc_err);
  } else {
    // NDEF URI record for "https://example.com".
    // nfc_t2t_payload_set expects NDEF payload bytes in RAM.
    static uint8_t ndef_msg[] = {
      0xD1, 0x01, 0x0C, 0x55, 0x04,
      'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm'
    };

    nfc_err = nfc_t2t_payload_set(ndef_msg, sizeof(ndef_msg));
    if (nfc_err != 0) {
      Serial.print("NFC payload set failed: ");
      Serial.println(nfc_err);
    } else {
      nfc_err = nfc_t2t_emulation_start();
      if (nfc_err != 0) {
        Serial.print("NFC emulation start failed: ");
        Serial.println(nfc_err);
      }
    }
  }
}

void loop() 
{
  if(digitalRead(7))
    Serial.println("1");

  if (Serial.available() > 0)
  {
    char c = Serial.read();
    if(c == 'o')
      lowPowerTest = true;
  }

  batteryUpdate(bleGetVbatThres());
  if (((batteryIsLow() == true) && (vbusConnected() == false)) || lowPowerTest || ((getBatteryVoltage() < 3.73f) && (getBatteryVoltage() > 0.2))) {
    // Immediately stop LEDs
    analogWrite(LED_CHANNEL, 0);
    //analogWrite(LED_BUILTIN, pwmMax);
    digitalWrite(LED_BUILTIN, HIGH);   // off (active-low)

    if(((getBatteryVoltage() < 3.73f) && (getBatteryVoltage() > 0.2)) || lowPowerTest)    //if battery is very low, completly turn off
    {
      if(lowBattery == false)
      {
        lowBattery = true;
                
        pinMode(LED_CHANNEL, INPUT_PULLDOWN);  //analogWrite(LED_CHANNEL, 0); 
        // pinMode(LED_BUILTIN, INPUT);   // off
        // #ifdef USE_PDM
        // PDM.end();
        // #endif
        // ble_sleep();

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
  else
  {
    // Wait for samples to be read
    if (samplesRead) 
    {
      updateLEDs();

#ifdef USE_BLE
      bleSetVBat_mV(getBatteryVoltage_mV());
      bleUpdate();
      brightness = bleGetBrightness();
      sensitivityF = bleGetSensitivity();
#endif
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
  
  // Read into the sample buffer
  PDM.read(sampleBuffer, bytesAvailable);
  
  // 16-bit, 2 bytes per sample
  samplesRead = bytesAvailable / 2;
}
#endif
