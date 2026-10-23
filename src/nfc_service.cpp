#include "nfc_service.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#ifndef ENABLE_NFC
#define ENABLE_NFC 0
#endif

#if ENABLE_NFC
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

extern "C" {
#include "nfc_t2t_lib.h"
}

using namespace Adafruit_LittleFS_Namespace;
#endif

namespace {

#if ENABLE_NFC
constexpr char DEFAULT_URI[] =
    "https://wa.me/41774321412?text=Hey%20Chris%2C%20cool%20necklace%21";
constexpr char URI_SETTINGS_FILE[] = "/nfc_uri.txt";
constexpr uint32_t EFFECT_DURATION_MS = 4000UL;
constexpr float EFFECT_FREQUENCY_HZ = 0.8f;
constexpr uint16_t EFFECT_MIN = 5;
constexpr uint16_t EFFECT_MAX = 250;

char currentUri[NfcService::MAX_URI_LENGTH + 1] = {0};
uint8_t ndefMessage[NfcService::MAX_URI_LENGTH + 6] = {0};
bool settingsLoaded = false;
bool emulationStarted = false;
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

bool validUri(const char *value, size_t length)
{
  if (value == nullptr || length == 0 ||
      length > NfcService::MAX_URI_LENGTH) {
    return false;
  }

  const bool isHttp = length > 7 && strncmp(value, "http://", 7) == 0;
  const bool isHttps = length > 8 && strncmp(value, "https://", 8) == 0;
  if (!isHttp && !isHttps) return false;

  for (size_t i = 0; i < length; ++i) {
    const uint8_t character = (uint8_t)value[i];
    if (character <= 0x20 || character == 0x7F) return false;
  }
  return true;
}

void splitUriForNdef(const char *value, uint8_t &prefix,
                     const char *&remainder)
{
  if (strncmp(value, "https://www.", 12) == 0) {
    prefix = 0x02;
    remainder = value + 12;
  } else if (strncmp(value, "http://www.", 11) == 0) {
    prefix = 0x01;
    remainder = value + 11;
  } else if (strncmp(value, "https://", 8) == 0) {
    prefix = 0x04;
    remainder = value + 8;
  } else {
    prefix = 0x03;
    remainder = value + 7;
  }
}

bool updatePayload(const char *value)
{
  uint8_t prefix = 0;
  const char *remainder = nullptr;
  splitUriForNdef(value, prefix, remainder);

  size_t ndefLength = 0;
  if (!buildNdefUriRecord(remainder, prefix, ndefMessage,
                          sizeof(ndefMessage), ndefLength)) {
    return false;
  }

  if (emulationStarted) {
    if (!resultOk(nfc_t2t_emulation_stop())) return false;
    emulationStarted = false;
    if (!resultOk(nfc_t2t_payload_set(nullptr, 0))) return false;
  }

  if (!resultOk(nfc_t2t_payload_set(ndefMessage, ndefLength))) return false;
  if (!resultOk(nfc_t2t_emulation_start())) return false;
  emulationStarted = true;
  return true;
}

bool saveUri()
{
  if (!InternalFS.begin()) return false;
  if (InternalFS.exists(URI_SETTINGS_FILE)) {
    InternalFS.remove(URI_SETTINGS_FILE);
  }

  File file(InternalFS);
  if (!file.open(URI_SETTINGS_FILE, FILE_O_WRITE)) return false;
  const size_t length = strlen(currentUri);
  const size_t written = file.write((const uint8_t *)currentUri, length);
  file.close();
  return written == length;
}

void loadUri()
{
  strcpy(currentUri, DEFAULT_URI);
  if (!InternalFS.begin() || !InternalFS.exists(URI_SETTINGS_FILE)) return;

  File file(InternalFS);
  if (!file.open(URI_SETTINGS_FILE, FILE_O_READ)) return;
  const int bytesRead =
      file.read((uint8_t *)currentUri, NfcService::MAX_URI_LENGTH);
  file.close();
  if (bytesRead <= 0 || bytesRead > (int)NfcService::MAX_URI_LENGTH) {
    strcpy(currentUri, DEFAULT_URI);
    return;
  }
  currentUri[bytesRead] = '\0';

  if (!validUri(currentUri, (size_t)bytesRead)) strcpy(currentUri, DEFAULT_URI);
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

void prepare()
{
#if ENABLE_NFC
  if (settingsLoaded) return;
  loadUri();
  settingsLoaded = true;
#endif
}

void begin()
{
#if ENABLE_NFC
  prepare();
  int result = nfc_t2t_setup(eventCallback, nullptr);
  if (!resultOk(result)) return;

  if (!updatePayload(currentUri)) Serial.println("NFC URI setup failed");
#endif
}

const char *uri()
{
#if ENABLE_NFC
  prepare();
  return currentUri;
#else
  return "";
#endif
}

bool setUri(const char *newUri, size_t length)
{
#if ENABLE_NFC
  if (!validUri(newUri, length)) return false;
  if (strlen(currentUri) == length &&
      memcmp(currentUri, newUri, length) == 0) {
    return true;
  }

  char candidate[MAX_URI_LENGTH + 1];
  memcpy(candidate, newUri, length);
  candidate[length] = '\0';

  char previous[MAX_URI_LENGTH + 1];
  strcpy(previous, currentUri);
  if (!updatePayload(candidate)) return false;

  memcpy(currentUri, candidate, length + 1);
  if (!saveUri()) {
    Serial.println("NFC URI save failed");
    updatePayload(previous);
    strcpy(currentUri, previous);
    saveUri();
    return false;
  }
  return true;
#else
  (void)newUri;
  (void)length;
  return false;
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
