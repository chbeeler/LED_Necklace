#pragma once
#include <Arduino.h>
#include <bluefruit.h>

void bleInit(uint16_t initialBrightness);
void bleUpdate();
void ble_sleep();
uint16_t bleGetBrightness();   // current brightness from BLE
void bleSetVBat_mV(uint16_t mv);  // push battery reading to characteristic
float bleGetVbatThres();
uint8_t getLedMode();
float getFilterAlpha();
float bleGetSensitivity();
float bleGetNoiseFloor();
float bleGetAeqFilterAlpha();
void bleSetVolMinMax(uint16_t vol_min, uint16_t vol_max);

// Potentiometer mode values, stored by the firmware in internal flash.
#define POT_MODE_DISABLED       0
#define POT_MODE_BRIGHTNESS     1
#define POT_MODE_EQ_SENSITIVITY 2

uint8_t bleGetPotMode();
void bleSetBrightnessFromPot(uint16_t value);
void bleSetSensitivityFromPot(uint16_t rawValue);
