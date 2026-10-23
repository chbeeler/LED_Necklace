#include "ble_control.h"
#include <Arduino.h>
#include <nrf_soc.h>

BLEUart bleuart;   // global

// ===================== UUIDs =====================
static BLEService ledService("19B10050-E8F2-537E-4F6C-D104768A1214");

static BLECharacteristic brightnessCharacteristic("19B10051-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic sensitivityCharacteristic("19B10052-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic debugCharacteristic("19B10053-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic vBatThresCharacteristic("19B10054-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic vBatCharacteristic("19B10055-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic noiseFloorCharacteristic("19B10056-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic filterCharacteristic("19B10100-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic settingsStrCharacteristic("19B10110-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic volMinMaxCharacteristic("19B10120-E8F2-537E-4F6C-D104768A1214");
static BLECharacteristic aeqFilterCharacteristic("19B10130-E8F2-537E-4F6C-D104768A1214");

// ===================== Your config/state =====================
#define SENSITIVITY_CONV_FACTOR   (0.0005f)

static uint16_t brightness = 100;
static float    sensitivityF = 0.5f;
static uint8_t  ledMode = 1;
static float    filter_alpha = 0.15f;
static float    aeq_filter_alpha = 0.15f;
static float    noise_floor  = 10.0f;
static float    vbatThres    = 3.73f;
static uint16_t vbat_mV      = 0;
static uint16_t vol_min      = 0;
static uint16_t vol_max      = 0;

static const uint8_t MODE_EQ       = 1;
static const uint8_t MODE_CONSTANT = 2;
static const uint8_t MODE_OFF      = 0;
static const uint8_t MODE_AEQ      = 3;
static const uint8_t MODE_POWEROFF = 9;
static const uint8_t MODE_BOOTLOADER = 55;
static const uint8_t DFU_MAGIC_UF2_RESET = 0x57;

// ===== flags / buffers filled by BLE callbacks =====
static volatile bool g_brightness_written = false;
static volatile bool g_sensitivity_written = false;
static volatile bool g_noise_floor_written = false;
static volatile bool g_filter_written = false;
static volatile bool g_vbatThres_written = false;
static volatile bool g_mode_written = false;
static volatile bool g_settings_written = false;
static volatile bool g_aeq_filter_written = false;

static volatile uint32_t g_brightness_u32 = 0;
static volatile uint32_t g_sensitivity_u32 = 0;
static volatile uint32_t g_noise_floor_u32 = 0;
static volatile uint32_t g_filter_u32 = 0;
static volatile uint32_t g_vbatThres_u32 = 0;
static volatile uint8_t  g_mode_u8 = 0;
static volatile uint32_t g_aeq_filter_u32 = 0;

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
static void cb_aeq_filter(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  g_aeq_filter_u32 = rd_u32_le(data, len);
  g_aeq_filter_written = true;
}
static void cb_mode(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  if (len >= 1) {
    g_mode_u8 = data[0];
    g_mode_written = true;
  }
}
static void cb_noise_floor(uint16_t /*conn_hdl*/, BLECharacteristic* /*chr*/, uint8_t* data, uint16_t len)
{
  g_noise_floor_u32 = rd_u32_le(data, len);
  g_noise_floor_written = true;
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

static void resetToUf2Bootloader(void)
{
  // Prefer SoftDevice API when BLE stack is active.
  uint32_t err = sd_power_gpregret_set(0, DFU_MAGIC_UF2_RESET);
  if (err != NRF_SUCCESS) {
    NRF_POWER->GPREGRET = DFU_MAGIC_UF2_RESET;
  }
  __DSB();
  __ISB();
  NVIC_SystemReset();
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

  // Use last byte (or last 2 bytes) as a stable differentiator
  ble_gap_addr_t addr = Bluefruit.getAddr();
  char name[32];
  snprintf(name, sizeof(name), "LED Necklace %02X%02X", addr.addr[4], addr.addr[5]);

  Bluefruit.setName(name);

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

  // noiseFloor (uint32), read/write
  noiseFloorCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  noiseFloorCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  noiseFloorCharacteristic.setFixedLen(4);
  noiseFloorCharacteristic.setWriteCallback(cb_noise_floor);
  noiseFloorCharacteristic.setUserDescriptor("Noise Floor raw (0..1000)");
  noiseFloorCharacteristic.begin();

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
  debugCharacteristic.setUserDescriptor("Mode (0=Off,1=EQ,2=On,3=AEQ,4=Firefly,9=PowerOff)");
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
  filterCharacteristic.setUserDescriptor("Filter speed (0..255)");
  filterCharacteristic.begin();

  // settings string (variable length <= 64)
  settingsStrCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  settingsStrCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  settingsStrCharacteristic.setMaxLen(64);
  settingsStrCharacteristic.setWriteCallback(cb_settings);
  settingsStrCharacteristic.setUserDescriptor("Settings: bm,s,a,nf,vbat");
  settingsStrCharacteristic.begin();

  // volume min/max (uint16 min, uint16 max), read-only
  volMinMaxCharacteristic.setProperties(CHR_PROPS_READ);
  volMinMaxCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  volMinMaxCharacteristic.setFixedLen(4);
  volMinMaxCharacteristic.setUserDescriptor("Audio volume range (min,max)");
  volMinMaxCharacteristic.begin();

  // AEQ filter (uint32), read/write (0..255)
  aeqFilterCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  aeqFilterCharacteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  aeqFilterCharacteristic.setFixedLen(4);
  aeqFilterCharacteristic.setWriteCallback(cb_aeq_filter);
  aeqFilterCharacteristic.setUserDescriptor("AEQ smoothing (0..255)");
  aeqFilterCharacteristic.begin();

  // --- Initial values ---
  // NOTE: All u32 values are little-endian when written by apps/web
  brightnessCharacteristic.write32(brightness);
  sensitivityCharacteristic.write32((uint32_t)(sensitivityF / SENSITIVITY_CONV_FACTOR + 0.5f));
  noiseFloorCharacteristic.write32((uint32_t)noise_floor);
  filterCharacteristic.write32((uint32_t)(filter_alpha * 500.0f + 0.5f));
  aeqFilterCharacteristic.write32((uint32_t)(aeq_filter_alpha * 255.0f + 0.5f));
  debugCharacteristic.write8(ledMode);
  vBatThresCharacteristic.write32((uint32_t)(vbatThres * 10.0f + 0.5f));
  vBatCharacteristic.write16(vbat_mV);
  bleSetVolMinMax(vol_min, vol_max);

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
  bool settings_dirty = false;

  // Apply pending writes latched by callbacks
  if (g_brightness_written) {
    g_brightness_written = false;
    uint32_t v = g_brightness_u32;
    if (v > 4095) v = 4095;
    brightness = (uint16_t)v;
    brightnessCharacteristic.write32(brightness);
    settings_dirty = true;
  }

  if (g_sensitivity_written) {
    g_sensitivity_written = false;
    uint32_t raw = g_sensitivity_u32;
    if (raw > 4095) raw = 4095;
    sensitivityF = (float)raw * SENSITIVITY_CONV_FACTOR;
    sensitivityCharacteristic.write32(raw);
    settings_dirty = true;
  }

  if (g_noise_floor_written) {
    g_noise_floor_written = false;
    uint32_t raw = g_noise_floor_u32;
    if (raw > 1000) raw = 1000;
    noise_floor = (float)raw;
    noiseFloorCharacteristic.write32(raw);
    settings_dirty = true;
  }

  if (g_filter_written) {
    g_filter_written = false;
    uint32_t raw = g_filter_u32;
    if (raw > 255) raw = 255;
    filter_alpha = (float)raw / 500.0f;
    filterCharacteristic.write32(raw);
    settings_dirty = true;
  }

  if (g_vbatThres_written) {
    g_vbatThres_written = false;
    vbatThres = (float)g_vbatThres_u32 / 10.0f;
    vBatThresCharacteristic.write32((uint32_t)(vbatThres * 10.0f + 0.5f));
  }

  if (g_aeq_filter_written) {
    g_aeq_filter_written = false;
    uint32_t raw = g_aeq_filter_u32;
    if (raw > 255) raw = 255;
    aeq_filter_alpha = (float)raw / 255.0f;
    aeqFilterCharacteristic.write32(raw);
  }

  if (g_mode_written) {
    g_mode_written = false;
    if (g_mode_u8 == MODE_BOOTLOADER) {
      debugCharacteristic.write8(g_mode_u8);
      resetToUf2Bootloader();
      return;
    }

    ledMode = g_mode_u8;
    debugCharacteristic.write8(ledMode);
    if (g_mode_u8 == MODE_POWEROFF)
      lowPowerTest = true;
    settings_dirty = true;
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

      settings_dirty = true;
    }
  }

  if (settings_dirty) {
    writeSettingsString();
  }
}

uint16_t bleGetBrightness()         { return brightness; }
void bleSetBrightnessFromButtons(uint16_t value)
{
  if (value > 4095) value = 4095;
  brightness = value;
  brightnessCharacteristic.write32(brightness);
  writeSettingsString();
}

void bleSetModeFromButtons(uint8_t mode)
{
  ledMode = mode;
  debugCharacteristic.write8(ledMode);
  writeSettingsString();
}

float    bleGetSensitivity()        { return sensitivityF; }
float    bleGetVbatThres()          { return vbatThres; }
uint8_t  getLedMode()               { return ledMode; }
float    getFilterAlpha()           { return filter_alpha; }
float    bleGetNoiseFloor()         { return noise_floor; }
float    bleGetAeqFilterAlpha()     { return aeq_filter_alpha; }

void bleSetVBat_mV(uint16_t mv)
{
  if (mv == vbat_mV) {
    return;
  }

  vbat_mV = mv;
  vBatCharacteristic.write16(mv);
}

void bleSetVolMinMax(uint16_t min_val, uint16_t max_val)
{
  vol_min = min_val;
  vol_max = max_val;

  uint8_t payload[4];
  payload[0] = (uint8_t)(vol_min & 0xFF);
  payload[1] = (uint8_t)((vol_min >> 8) & 0xFF);
  payload[2] = (uint8_t)(vol_max & 0xFF);
  payload[3] = (uint8_t)((vol_max >> 8) & 0xFF);
  volMinMaxCharacteristic.write(payload, sizeof(payload));
}
