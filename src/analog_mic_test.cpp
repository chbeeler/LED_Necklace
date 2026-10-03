#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

// Analog microphone test mode. The nRF52840 ADC setting used by this core maps
// the 12-bit values 0..4095 to the nominal 0..3.6 V input range. Output is CSV:
//
//   timestamp_us,raw_adc,low_passed_adc
//
// The integer IIR uses alpha=1/16. It is intentionally cheap so the terminal's
// serial throughput, rather than floating-point math or a delay, sets the rate.
static const uint8_t ANALOG_MIC_PIN = PIN_A1; // XIAO A1 = nRF52840 P0.03
static const uint8_t ANALOG_MIC_ADC_BITS = 12;
static const uint8_t ANALOG_MIC_LP_SHIFT = 4; // alpha = 1 / (2^4) = 0.0625
static int32_t analogMicFilteredQ8 = 0;
static bool analogMicFilterInitialized = false;

void setup()
{
  Serial.begin(921600);
  pinMode(ANALOG_MIC_PIN, INPUT);
  analogReference(AR_DEFAULT); // Nominal 3.6 V ADC range on this board/core.
  analogReadResolution(ANALOG_MIC_ADC_BITS);
}

void loop()
{
  // Do not fill/block the USB serial buffer before a terminal is connected.
  if (!Serial) return;

  int raw = analogRead(ANALOG_MIC_PIN);
  int32_t targetQ8 = (int32_t)raw << 8;

  if (!analogMicFilterInitialized) {
    analogMicFilteredQ8 = targetQ8;
    analogMicFilterInitialized = true;
  } else {
    analogMicFilteredQ8 +=
        (targetQ8 - analogMicFilteredQ8) >> ANALOG_MIC_LP_SHIFT;
  }

  int filtered = (int)((analogMicFilteredQ8 + 128) >> 8);
  Serial.print(micros());
  Serial.print(',');
  Serial.print(raw);
  Serial.print(',');
  Serial.println(filtered);
}
