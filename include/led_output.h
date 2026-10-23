#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace LedOutput {

constexpr uint8_t PWM_RESOLUTION_BITS = 12;
constexpr uint16_t PWM_MAX = (1U << PWM_RESOLUTION_BITS) - 1U;

void begin();
void write(uint32_t value);
void setPinMode(uint8_t mode);

}  // namespace LedOutput
