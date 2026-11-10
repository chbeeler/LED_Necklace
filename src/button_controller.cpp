#include "button_controller.h"

#include <Arduino.h>

namespace {

constexpr uint8_t PLUS_DRIVE_PIN = D7;
constexpr uint8_t PLUS_SENSE_PIN = D9;
constexpr uint8_t MINUS_DRIVE_PIN = D4;
constexpr uint8_t MINUS_SENSE_PIN = D6;
ButtonGestures gestures;

}  // namespace

namespace ButtonController {

void begin()
{
  // Set each output latch before enabling output mode to avoid a LOW glitch.
  digitalWrite(PLUS_DRIVE_PIN, HIGH);
  pinMode(PLUS_DRIVE_PIN, OUTPUT);
  pinMode(PLUS_SENSE_PIN, INPUT_PULLDOWN);

  digitalWrite(MINUS_DRIVE_PIN, HIGH);
  pinMode(MINUS_DRIVE_PIN, OUTPUT);
  pinMode(MINUS_SENSE_PIN, INPUT_PULLDOWN);
}

bool plusPressedRaw()
{
  return digitalRead(PLUS_SENSE_PIN) == HIGH;
}

bool minusPressedRaw()
{
  return digitalRead(MINUS_SENSE_PIN) == HIGH;
}

ButtonAction update(uint32_t nowMs)
{
  const bool plusWasPressed = gestures.plusPressed();
  const bool minusWasPressed = gestures.minusPressed();
  const ButtonAction action = gestures.update(plusPressedRaw(), minusPressedRaw(), nowMs);
  if (!plusWasPressed && gestures.plusPressed()) Serial.println("Plus (D7-D9) pressed");
  if (!minusWasPressed && gestures.minusPressed()) Serial.println("Minus (D4-D6) pressed");
  if (action == ButtonAction::RequestSystemOff) Serial.println("Both buttons held: system off requested");
  return action;
}

void waitForRelease()
{
  uint32_t releasedSinceMs = 0;
  while (true) {
    if (plusPressedRaw() || minusPressedRaw()) {
      releasedSinceMs = 0;
    } else if (releasedSinceMs == 0) {
      releasedSinceMs = millis();
    } else if ((uint32_t)(millis() - releasedSinceMs) >=
               RELEASE_GUARD_MS) {
      return;
    }
    delay(5);
  }
}

void configureWakeSources()
{
  digitalWrite(PLUS_DRIVE_PIN, HIGH);
  pinMode(PLUS_DRIVE_PIN, OUTPUT);
  digitalWrite(MINUS_DRIVE_PIN, HIGH);
  pinMode(MINUS_DRIVE_PIN, OUTPUT);
  pinMode(PLUS_SENSE_PIN, INPUT_PULLDOWN_SENSE);
  pinMode(MINUS_SENSE_PIN, INPUT_PULLDOWN_SENSE);

  NRF_P0->LATCH = 0xFFFFFFFFUL;
  NRF_P1->LATCH = 0xFFFFFFFFUL;
}

}  // namespace ButtonController
