#include "ble_control.h"
#include <Arduino.h>

BLEUart bleuart;   // global

// ===================== UUIDs =====================
// Keep same UUID strings as your ArduinoBLE version
#ifdef DEV2
static BLEService ledService("18C30050-E8F2-537E-4F6C-D104768A1214");

static BLECharacteristic brightnessCharacteristic("18C30041-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic sensitivityCharacteristic("18C30042-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic vBatCharacteristic("18C30052-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic debugCharacteristic("18C30053-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic vBatThresCharacteristic("18C30054-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic filterCharacteristic("18C30100-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic settingsStrCharacteristic("18C30110-E8F2-537E-4F6C-D104768A1214");

#else
static BLEService ledService("18C40050-E8F2-537E-4F6C-D104768A1214");

static BLECharacteristic brightnessCharacteristic("18C40041-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic sensitivityCharacteristic("18C40042-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic vBatCharacteristic("18C40052-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic debugCharacteristic("18C40053-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic vBatThresCharacteristic("18C40054-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic filterCharacteristic("18C40100-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic settingsStrCharacteristic("18C40110-E8F2-537E-4F6C-D104768A1214");
#endif

// ===================== Your config/state =====================
#define SENSITIVITY_CONV_FACTOR   (0.0005f)

static uint16_t brightness = 100;
static float    sensitivityF = 0.5f;
static uint8_t  ledMode = 1;
static float    filter_alpha = 0.15f;
static float    noise_floor  = 10.0f;
static float    vbatThres    = 3.73f;
static uint16_t vbat_mV      = 0;

static const uint8_t MODE_EQ       = 1;
static const uint8_t MODE_CONSTANT = 2;

// ===== flags / buffers filled by BLE callbacks =====
static volatile bool g_brightness_written = false;
static volatile bool g_sensitivity_written = false;
static volatile bool g_filter_written = false;
static volatile bool g_vbatThres_written = false;
static volatile bool g_mode_written = false;
static volatile bool g_settings_written = false;

static volatile uint32_t g_brightness_u32 = 0;
static volatile uint32_t g_sensitivity_u32 = 0;
static volatile uint32_t g_filter_u32 = 0;
static volatile uint32_t g_vbatThres_u32 = 0;
static volatile uint8_t  g_mode_u8 = 0;

static char g_settings_buf[65]; // 64 + NUL

// ===================== helpers =====================
static uint32_t rd_u32_le(const uint8_t* d, uint16_t len)
{
  if (len < 4) return 0;
  return (uint32_t)d[0] |
         ((uint32_t)d[1] << 8) |
         ((uint32_t)d[2] << 16) |
         ((uint32_t)d[3] << 24);
}

static void writeSettingsString()
{
  // bm encodes mode in sign
  int bm = (int)brightness;
  if (ledMode == MODE_CONSTANT) bm = -bm;

  int sensitivityRaw  = (int)(sensitivityF / SENSITIVITY_CONV_FACTOR + 0.5f);
  int alphaRaw        = (int)(filter_alpha * 500.0f + 0.5f);
  int noiseFloorRaw   = (int)(noise_floor + 0.5f);

  // clamp ranges
  if (bm > 4095) bm = 4095;
  if (bm < -4095) bm = -4095;
  if (sensitivityRaw < 0) sensitivityRaw = 0;
  if (sensitivityRaw > 4095) sensitivityRaw = 4095;
  if (alphaRaw < 0) alphaRaw = 0;
  if (alphaRaw > 255) alphaRaw = 255;
  if (noiseFloorRaw < 0) noiseFloorRaw = 0;
  if (noiseFloorRaw > 255) noiseFloorRaw = 255;

  char s[64];
  snprintf(s, sizeof(s), "%d,%d,%d,%d,%d", bm, sensitivityRaw, alphaRaw, noiseFloorRaw, vbat_mV);

  //Serial.println(s);

  // write string characteristic (variable length)
  settingsStrCharacteristic.write((const uint8_t*)s, (uint16_t)strlen(s));
}

// ===================== callbacks =====================
static void cb_brightness(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  g_brightness_u32 = rd_u32_le(data, len);
  g_brightness_written = true;
}
static void cb_sensitivity(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  g_sensitivity_u32 = rd_u32_le(data, len);
  g_sensitivity_written = true;
}
static void cb_filter(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  g_filter_u32 = rd_u32_le(data, len);
  g_filter_written = true;
}
static void cb_vbatThres(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  g_vbatThres_u32 = rd_u32_le(data, len);
  g_vbatThres_written = true;
}
static void cb_mode(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  if (len >= 1) {
    g_mode_u8 = data[0];
    g_mode_written = true;
  }
}
static void cb_settings(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  if (len > 64) len = 64;
  memcpy((void*)g_settings_buf, data, len);
  g_settings_buf[len] = '\0';
  Serial.println(g_settings_buf);
  g_settings_written = true;
}

static void disconnectBle() {
  uint16_t connections = Bluefruit.connected();
  for (uint16_t conn = 0; conn < connections; conn++) {
    Bluefruit.disconnect(conn);
  }
}

void ble_sleep(void) {
  Bluefruit.Advertising.restartOnDisconnect(false);
  disconnectBle();
  Bluefruit.Advertising.stop();
}

// ===================== public API =====================
void bleInit(uint16_t initialBrightness)
{
  brightness = initialBrightness;

  Bluefruit.begin();

  Bluefruit.autoConnLed(false);   // disable the blinking connection LED
  Bluefruit.setConnLedInterval(0);  // optional: no LED updates

  #ifdef DEV2
  Bluefruit.setName("LED Necklace 2");
  #else
  Bluefruit.setName("LED Necklace 1");
  #endif

  bleuart.begin();

  // Optional (nice for iOS): faster re-adv on disconnect
  Bluefruit.Periph.setConnectCallback(nullptr);
  Bluefruit.Periph.setDisconnectCallback(nullptr);

  // --- Service ---
  ledService.begin();

  // --- Characteristics ---
  // brightness (uint32), read/write
  brightnessCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  brightnessCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  brightnessCharacteristic.setFixedLen(4);
  brightnessCharacteristic.setWriteCallback(cb_brightness);
  brightnessCharacteristic.setUserDescriptor("Brightness (0..4095)");
  brightnessCharacteristic.begin();

  // sensitivity (uint32), read/write
  sensitivityCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  sensitivityCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  sensitivityCharacteristic.setFixedLen(4);
  sensitivityCharacteristic.setWriteCallback(cb_sensitivity);
  sensitivityCharacteristic.setUserDescriptor("Sensitivity raw (0..4095)");
  sensitivityCharacteristic.begin();

  // vBat (uint16), read
  vBatCharacteristic.setProperties(CHR_PROPS_READ);
  vBatCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  vBatCharacteristic.setFixedLen(2);
  vBatCharacteristic.setUserDescriptor("Battery voltage (mV)");
  vBatCharacteristic.begin();

  // mode (uint8), read/write
  debugCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  debugCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  debugCharacteristic.setFixedLen(1);
  debugCharacteristic.setWriteCallback(cb_mode);
  debugCharacteristic.setUserDescriptor("Mode (0=Off,1=EQ,2=Constant)");
  debugCharacteristic.begin();

  // vBatThres (uint32), read/write (V*10)
  vBatThresCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  vBatThresCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  vBatThresCharacteristic.setFixedLen(4);
  vBatThresCharacteristic.setWriteCallback(cb_vbatThres);
  vBatThresCharacteristic.setUserDescriptor("VBat threshold (V*10)");
  vBatThresCharacteristic.begin();

  // filter (uint32), read/write (0..255)
  filterCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  filterCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  filterCharacteristic.setFixedLen(4);
  filterCharacteristic.setWriteCallback(cb_filter);
  filterCharacteristic.setUserDescriptor("Filter amount (0..255)");
  filterCharacteristic.begin();

  // settings string (variable length <= 64)
  settingsStrCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  settingsStrCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  settingsStrCharacteristic.setMaxLen(64);
  settingsStrCharacteristic.setWriteCallback(cb_settings);
  settingsStrCharacteristic.setUserDescriptor("Settings: bm,s,a,nf,vbat");
  settingsStrCharacteristic.begin();

  // --- Initial values ---
  // NOTE: All u32 values are little-endian when written by apps/web
  brightnessCharacteristic.write32(brightness);
  sensitivityCharacteristic.write32((uint32_t)(sensitivityF / SENSITIVITY_CONV_FACTOR + 0.5f));
  filterCharacteristic.write32((uint32_t)(filter_alpha * 500.0f + 0.5f));
  debugCharacteristic.write8(ledMode);
  vBatThresCharacteristic.write32((uint32_t)(vbatThres * 10.0f + 0.5f));
  vBatCharacteristic.write16(vbat_mV);

  writeSettingsString();

  // --- Advertising ---
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(ledService);
  Bluefruit.ScanResponse.addName();

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244); // 20ms..152.5ms
  Bluefruit.Advertising.setFastTimeout(5);
  Bluefruit.Advertising.start(0); // 0 = forever
}
extern bool lowPowerTest;
void bleUpdate()
{
  // Apply pending writes latched by callbacks
  if (g_brightness_written) {
    g_brightness_written = false;
    uint32_t v = g_brightness_u32;
    if (v > 4095) v = 4095;
    brightness = (uint16_t)v;
    brightnessCharacteristic.write32(brightness);
    writeSettingsString();
  }

  if (g_sensitivity_written) {
    g_sensitivity_written = false;
    uint32_t raw = g_sensitivity_u32;
    if (raw > 4095) raw = 4095;
    sensitivityF = (float)raw * SENSITIVITY_CONV_FACTOR;
    sensitivityCharacteristic.write32(raw);
    writeSettingsString();
  }

  if (g_filter_written) {
    g_filter_written = false;
    uint32_t raw = g_filter_u32;
    if (raw > 255) raw = 255;
    filter_alpha = (float)raw / 500.0f;
    filterCharacteristic.write32(raw);
    writeSettingsString();
  }

  if (g_vbatThres_written) {
    g_vbatThres_written = false;
    vbatThres = (float)g_vbatThres_u32 / 10.0f;
    vBatThresCharacteristic.write32((uint32_t)(vbatThres * 10.0f + 0.5f));
  }

  if (g_mode_written) {
    g_mode_written = false;
    ledMode = g_mode_u8;
    debugCharacteristic.write8(ledMode);
    if(g_mode_u8 == 3)
      lowPowerTest = true;
    writeSettingsString();
  }

  if (g_settings_written) {
    g_settings_written = false;

    // Expect "bm,s,a,nf" (your existing format)
    int bm = 0, b = 0, s = 0, a = 0, nfRaw = 0;
    int matched = sscanf(g_settings_buf, " %d , %d , %d , %d ", &bm, &s, &a, &nfRaw);
    if (matched == 4) {
      if (bm < 0) { ledMode = MODE_CONSTANT; b = -bm; }
      else        { ledMode = MODE_EQ;       b = bm;  }

      if (b < 0) b = 0; 
      if (b > 4095) b = 4095;
      if (s < 0) s = 0; 
      if (s > 4095) s = 4095;
      if (a < 0) a = 0; 
      if (a > 255)  a = 255;
      if (nfRaw < 0) nfRaw = 0; 
      if (nfRaw > 255) nfRaw = 255;

      brightness   = (uint16_t)b;
      sensitivityF = (float)s * SENSITIVITY_CONV_FACTOR;
      filter_alpha = (float)a / 500.0f;
      noise_floor  = (float)nfRaw;

      // sync other characteristics
      brightnessCharacteristic.write32(brightness);
      sensitivityCharacteristic.write32((uint32_t)s);
      filterCharacteristic.write32((uint32_t)a);
      debugCharacteristic.write8(ledMode);

      writeSettingsString();
    }
  }
}

uint16_t bleGetBrightness()         { return brightness; }
float    bleGetSensitivity()        { return sensitivityF; }
float    bleGetVbatThres()          { return vbatThres; }
uint8_t  getLedMode()               { return ledMode; }
float    getFilterAlpha()           { return filter_alpha; }
float    bleGetNoiseFloor()         { return noise_floor; }

void bleSetVBat_mV(uint16_t mv)
{
  vbat_mV = mv;
  vBatCharacteristic.write16(mv);
  writeSettingsString(); // same as your TODO: not efficient, but identical behaviour
}
