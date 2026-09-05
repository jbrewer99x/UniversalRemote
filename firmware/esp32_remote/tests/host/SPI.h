#pragma once
#include "Arduino.h"
#include <cassert>
#include <vector>
#include <utility>
constexpr int FSPI = 0, MSBFIRST = 1, SPI_MODE0 = 0;
inline uint16_t testPixels[240 * 320]{};
inline size_t testTransferredPixels = 0;
inline std::vector<std::pair<uint8_t, uint32_t>> testLcdCommands;
struct SPISettings { SPISettings(int, int, int) {} };
class SPIClass {
public:
    explicit SPIClass(int) {}
    void begin(int, int, int, int) {}
    void beginTransaction(SPISettings) {}
    void endTransaction() {}
    void transfer(uint8_t byte) {
        if (testPins[41] == LOW) { command = byte; testLcdCommands.emplace_back(byte, millis()); }
    }
    void writeBytes(uint8_t* bytes, size_t size) {
        if (command == 0x2A) { x0 = bytes[0] * 256 + bytes[1]; x1 = bytes[2] * 256 + bytes[3]; }
        else if (command == 0x2B) { y0 = bytes[0] * 256 + bytes[1]; y1 = bytes[2] * 256 + bytes[3]; }
        else if (command == 0x2C) {
            assert(size == size_t((x1 - x0 + 1) * (y1 - y0 + 1) * 2));
            assert(x0 >= 0 && x1 < 240 && y0 >= 0 && y1 < 320);
            size_t index = 0;
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                testPixels[y * 240 + x] = bytes[index] * 256 + bytes[index + 1];
                index += 2;
            }
            testTransferredPixels += size / 2;
        }
    }
private:
    uint8_t command = 0;
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};
