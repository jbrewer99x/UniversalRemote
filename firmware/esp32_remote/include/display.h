#pragma once

#include <Arduino.h>

void initDisplay();
void displayStatus(
    bool wifiConnected,
    const String &ipAddress,
    const String &otaStatus
);
void setDisplayBrightness(uint8_t percent);