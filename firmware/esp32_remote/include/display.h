#pragma once

#include <Arduino.h>

bool initDisplay();
struct DisplayAction {
    const char* name;
    int value;
};
void setDisplayTouch(bool pressed, uint16_t x, uint16_t y);
void suppressDisplayTouch();
void serviceDisplay();
void flushDisplay();
bool takeDisplayAction(DisplayAction &action);
void displayFeedback(const char* message);
void printDisplayStats();
void setDisplaySleeping(bool sleeping);
void serviceDisplayPower();
bool isDisplaySleeping();
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
