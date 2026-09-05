#pragma once

#include <Arduino.h>

void initDisplay();
void updateWifiStatus(bool connected);
void updateDeviceSelector(bool pcSelected);
void displayStatus(
    bool wifiConnected,
    const String &ipAddress,
    const String &otaStatus,
    bool pcSelected
);
void setDisplayBrightness(uint8_t percent);

void displaySettings(
    uint8_t brightnessPercent,
    uint16_t sleepSeconds
);
void displayUpdateStatus(const char* message);
void updateBrightnessSlider(uint8_t brightnessPercent);
void updateSleepSlider(uint16_t sleepSeconds);
void updateBatteryStatus(uint8_t percent, float volts);