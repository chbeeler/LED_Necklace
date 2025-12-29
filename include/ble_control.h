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
uint16_t getStaticBrightness();