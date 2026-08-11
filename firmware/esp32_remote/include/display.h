#pragma once

#include <Arduino.h>

void initDisplay();
void displayStatus(
    bool wifiConnected,
    const String &ipAddress,
    const String &otaStatus
);
void setDisplayBrightness(uint8_t percent);

void displaySettings(
    uint8_t brightnessPercent,
    uint16_t sleepSeconds
);
void updateBrightnessSlider(uint8_t brightnessPercent);
void updateSleepSlider(uint16_t sleepSeconds);
void updateBatteryStatus(uint8_t percent);