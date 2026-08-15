#pragma once

#include <Arduino.h>

bool sendRemoteCommand(
    const String &device,
    const String &command
);
bool sendRemoteCommand(
    const String &device,
    const String &command
);

bool checkRemoteCommand(
    String &command
);
bool checkRemoteCommand(String &command);