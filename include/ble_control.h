#pragma once
#include <Arduino.h>
#include <bluefruit.h>

struct AudioTelemetry;

void bleInit(uint16_t initialBrightness);
void bleUpdate();
void ble_sleep();
uint16_t bleGetBrightness();   // current brightness from BLE
void bleSetBrightnessFromButtons(uint16_t value);
void bleSetModeFromButtons(uint8_t mode);
void bleSetVBat_mV(uint16_t mv);  // push battery reading to characteristic
float bleGetVbatThres();
uint8_t getLedMode();
float getFilterAlpha();
float bleGetSensitivity();
float bleGetNoiseFloor();
float bleGetAeqFilterAlpha();
void bleSetVolMinMax(uint16_t vol_min, uint16_t vol_max);
void bleSetAudioTelemetry(const AudioTelemetry &telemetry);
