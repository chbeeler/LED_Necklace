#include "battery.h"
#include <Arduino.h>

// reuse your existing constants / pins here
// (or move them into this file and keep only API in the header)

// Battery measurement pins (XIAO nRF52840 Sense)
#ifndef VBAT_ENABLE
#define VBAT_ENABLE   P0_14
#endif
#define PIN_BAT_EN   VBAT_ENABLE   // P0.14, enables divider when LOW

// Divider values: 1M (top) and 510k (bottom)
const int VBAT_R1 = 1000;   // top resistor (to battery +)
int VBAT_R2 = 490;    // bottom resistor (to GND) (510?)

// ADC configuration (nRF52 core: AR_INTERNAL_1_8 = 1.8 V, 12-bit)
const float ADC_REF_V       = 1.8f;
const int   ADC_RES_BITS    = 12;

static unsigned long lastBatSampleMs = 0;
static bool  lowBattery = false;
static float lastVBat   = 0.0f;
static int lastVBatmV = 0;
static int raw = 0;
static int raw_prev1 = 0;
static int raw_prev2 = 0;

int measureVBatPinRaw()
{
  // Pot sampling changes the shared ADC configuration, so restore the
  // battery-divider range for every battery measurement.
  analogReference(AR_INTERNAL_1_8);
  analogReadResolution(ADC_RES_BITS);

  pinMode(PIN_BAT_EN, OUTPUT);
  digitalWrite(PIN_BAT_EN, LOW);
  
  delayMicroseconds(500);        // usually enough

  (void)analogRead(PIN_VBAT);    // discard 1st
  delayMicroseconds(200);

  // average a few (fast, low jitter)
  uint32_t sum = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) {
    sum += analogRead(PIN_VBAT);
    delayMicroseconds(100);
  }

  //Serial.print(sum / N);
  //Serial.print(",");

  pinMode(PIN_BAT_EN, INPUT);
  return (int)(sum / N);
}

void batteryInit()
{
  pinMode(PIN_BAT_EN, INPUT);
  analogReference(AR_INTERNAL_1_8);
  analogReadResolution(ADC_RES_BITS);
  raw_prev2 = measureVBatPinRaw();
  raw_prev1 = measureVBatPinRaw();  
  raw = measureVBatPinRaw();
}

bool vbusConnected()
{
  // USBREGSTATUS reports actual voltage on VBUS.  The charger's nCHARGING
  // output is not a reliable VBUS indicator when no battery is installed.
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

const float adcMax = (1 << ADC_RES_BITS) - 1;
void batteryUpdate(float vbatThres)
{
  unsigned long now = millis();
  if (lowBattery == true && vbusConnected() == false) return;

  if (now - lastBatSampleMs < 1000UL) return;   // 1 s

  raw_prev2 = raw_prev1;
  raw_prev1 = raw;  
  lastBatSampleMs = now;
  raw = measureVBatPinRaw();

  float vDiv   = (raw+raw_prev1+raw_prev2)/3 * ADC_REF_V / adcMax;
  lastVBat     = vDiv * ((VBAT_R1 + VBAT_R2) / (float)VBAT_R2);
  lastVBatmV   = (int)(lastVBat*1000.0f);

  if (lastVBat < vbatThres && vbusConnected() == false)
    lowBattery = true;
  else
    lowBattery = false;
}

bool batteryIsLow()
{
  return lowBattery;
}

float getBatteryVoltage()
{
  return lastVBat;
}

int getBatteryVoltage_mV()
{
  return lastVBatmV;
}
