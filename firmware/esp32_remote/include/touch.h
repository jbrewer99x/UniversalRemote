#pragma once

#include <Arduino.h>

struct RemoteTouchPoint {
    bool touched;
    uint16_t x;
    uint16_t y;
};

bool initTouch();
RemoteTouchPoint readTouch();