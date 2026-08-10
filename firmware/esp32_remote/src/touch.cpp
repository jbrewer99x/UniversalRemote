#include <Arduino.h>
#include <Wire.h>
#include <TouchDrv.hpp>

#include "touch.h"

#define TP_SDA   1
#define TP_SCL   3
#define TP_INT   4
#define TP_RST   2
#define TP_ADDR  0x58

static TwoWire touchWire = TwoWire(0);
static TouchDrvCST3530 touch;
static bool touchReady = false;

bool initTouch() {
    Serial.println("Touch: initializing CST3530");

    touch.setPins(TP_RST, TP_INT);

    touchReady = touch.begin(
        touchWire,
        TP_ADDR,
        TP_SDA,
        TP_SCL
    );

    if (!touchReady) {
        Serial.println("Touch: CST3530 initialization FAILED");
        return false;
    }

    Serial.printf(
        "Touch: %s initialized at 0x%02X\n",
        touch.getModelName(),
        TP_ADDR
    );

    Serial.printf(
        "Touch: supports %u point(s)\n",
        touch.getSupportTouchPoint()
    );

    return true;
}

RemoteTouchPoint readTouch() {
    RemoteTouchPoint result = {
        false,
        0,
        0
    };

    if (!touchReady) {
        return result;
    }

    // IRQ is active LOW. Don't hit I2C unless the controller
    // indicates that touch data is available.
    if (digitalRead(TP_INT) != LOW) {
        return result;
    }

    const TouchPoints &points = touch.getTouchPoints();

    if (!points.hasPoints()) {
        return result;
    }

    const auto &pt = points.getPoint(0);

    result.touched = true;
    result.x = 239 - constrain(pt.x, 0, 239);
    result.y = 319 - constrain(pt.y, 0, 319);

    static uint32_t lastPrint = 0;
    uint32_t now = millis();

    // Throttle serial output while dragging.
    if (now - lastPrint >= 50) {
        Serial.printf(
            "Touch: x=%u y=%u points=%u\n",
            result.x,
            result.y,
            points.getPointCount()
        );

        lastPrint = now;
    }

    return result;
}