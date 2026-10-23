#include "nfc_service.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#ifndef ENABLE_NFC
#define ENABLE_NFC 0
#endif

#if ENABLE_NFC
extern "C" {
#include "nfc_t2t_lib.h"
}
#endif

namespace {

#if ENABLE_NFC
constexpr uint8_t URI_PREFIX_HTTPS = 0x04;
constexpr char URI_TEXT[] =
    "wa.me/41774321412?text=Hey%20Chris%2C%20cool%20necklace%21";
constexpr uint32_t EFFECT_DURATION_MS = 4000UL;
constexpr float EFFECT_FREQUENCY_HZ = 0.8f;
constexpr uint16_t EFFECT_MIN = 5;
constexpr uint16_t EFFECT_MAX = 250;

volatile bool effectTriggered = false;
bool effectIsActive = false;
uint32_t effectStartMs = 0;

bool resultOk(int result)
{
  return result == 0 || (uint32_t)result == 0x0BAD0000UL;
}

bool buildNdefUriRecord(const char *uriText, uint8_t uriPrefix,
                        uint8_t *output, size_t outputSize,
                        size_t &outputLength)
{
  if (uriText == nullptr || output == nullptr) return false;

  const size_t uriLength = strlen(uriText);
  if (uriLength == 0U || uriLength + 1U > 255U) return false;

  const size_t recordLength = 5U + uriLength;
  if (recordLength > outputSize) return false;

  // NDEF short record: MB|ME|SR, TNF=well-known, type='U' (URI).
  output[0] = 0xD1;
  output[1] = 0x01;
  output[2] = (uint8_t)(uriLength + 1U);
  output[3] = 0x55;
  output[4] = uriPrefix;
  memcpy(&output[5], uriText, uriLength);

  outputLength = recordLength;
  return true;
}

void eventCallback(void *context, nfc_t2t_event_t event, const uint8_t *data,
                   size_t dataLength)
{
  (void)context;
  (void)data;
  (void)dataLength;

  if (event == NFC_T2T_EVENT_DATA_READ) effectTriggered = true;
}
#endif

}  // namespace

namespace NfcService {

void begin()
{
#if ENABLE_NFC
  int result = nfc_t2t_setup(eventCallback, nullptr);
  if (!resultOk(result)) return;

  // nfc_t2t_payload_set requires the NDEF payload to remain in RAM.
  static uint8_t ndefMessage[260];
  size_t ndefLength = 0;
  if (!buildNdefUriRecord(URI_TEXT, URI_PREFIX_HTTPS, ndefMessage,
                          sizeof(ndefMessage), ndefLength)) {
    Serial.println("NFC URI build failed");
    return;
  }

  result = nfc_t2t_payload_set(ndefMessage, ndefLength);
  if (resultOk(result)) nfc_t2t_emulation_start();
#endif
}

void update(uint32_t nowMs)
{
#if ENABLE_NFC
  if (effectTriggered) {
    effectTriggered = false;
    effectIsActive = true;
    effectStartMs = nowMs;
  }
#else
  (void)nowMs;
#endif
}

bool effectActive()
{
#if ENABLE_NFC
  return effectIsActive;
#else
  return false;
#endif
}

bool renderEffect(uint32_t nowMs, uint16_t &brightness)
{
#if ENABLE_NFC
  if (!effectIsActive) return false;

  const uint32_t elapsedMs = nowMs - effectStartMs;
  if (elapsedMs >= EFFECT_DURATION_MS) {
    effectIsActive = false;
    return false;
  }

  const float timeSeconds = (float)elapsedMs / 1000.0f;
  const float phase = 2.0f * PI * EFFECT_FREQUENCY_HZ * timeSeconds;
  const float wave = 0.5f * (sinf(phase) + 1.0f);
  brightness = EFFECT_MIN +
               (uint16_t)((float)(EFFECT_MAX - EFFECT_MIN) * wave);
  return true;
#else
  (void)nowMs;
  (void)brightness;
  return false;
#endif
}

}  // namespace NfcService
