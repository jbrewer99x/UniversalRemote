#pragma once

#include <Arduino.h>

bool initSdCard();
bool testSdCard();
bool writeSdTextFile(const char* path, const String& text);
String readSdTextFile(const char* path);
bool isSdCardReady();