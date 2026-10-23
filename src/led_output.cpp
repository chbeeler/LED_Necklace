#include "led_output.h"

namespace {

constexpr uint8_t LED_CHANNEL = D3;           // Existing necklace PCB.
constexpr uint8_t LED_CHANNEL_DUPLICATE = D0; // Easier to solder on new PCBs.

}  // namespace

namespace LedOutput {

void begin()
{
  analogWriteResolution(PWM_RESOLUTION_BITS);
  setPinMode(OUTPUT);
  write(0);
}

void write(uint32_t value)
{
  if (value > PWM_MAX) value = PWM_MAX;

  // Both pins use channels of the same PWM peripheral and carry the same
  // waveform, retaining compatibility with both necklace PCB revisions.
  analogWrite(LED_CHANNEL, value);
  analogWrite(LED_CHANNEL_DUPLICATE, value);
}

void setPinMode(uint8_t mode)
{
  pinMode(LED_CHANNEL, mode);
  pinMode(LED_CHANNEL_DUPLICATE, mode);
}

}  // namespace LedOutput
