#include "sound_effects.h"
#include <Arduino.h>
#include <Wire.h>
#include <TouchDrv.hpp>

#include "touch.h"
#include "ui_policy.h"

#define TP_SDA   1
#define TP_SCL   3
#define TP_INT   4
#define TP_RST   2
#define TP_ADDR  0x58

static TwoWire touchWire = TwoWire(0);
static TouchDrvCST3530 touch;
static bool touchReady = false;
static bool touchSleeping = false;
static TouchState touchState;
static RemoteTouchPoint lastPoint = {false, 0, 0};

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
        reportErrorSound();
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
    if (!touchReady || touchSleeping) return {false, 0, 0};
    auto sample = TouchState::Sample::None;
    // CST3530 reports data-ready, not finger-down, on IRQ. Retain a press
    // between reports; SensorLib filters the controller's release events.
    if (digitalRead(TP_INT) == LOW) {
        const auto &points = touch.getTouchPoints();
        if (points.hasPoints()) {
            const auto &point = points.getPoint(0);
            lastPoint.x = 239 - constrain(point.x, 0, 239);
            lastPoint.y = 319 - constrain(point.y, 0, 319);
            sample = TouchState::Sample::Down;
        } else sample = TouchState::Sample::Up;
    }
    lastPoint.touched = touchState.update(sample, millis());
    return lastPoint;
}

void setTouchSleeping(bool sleeping) {
    if (!touchReady || sleeping == touchSleeping) return;
    if (sleeping) touch.sleep();
    else touch.wakeup();
    touchSleeping = sleeping;
    touchState = TouchState{};
    lastPoint = {false, 0, 0};
}
