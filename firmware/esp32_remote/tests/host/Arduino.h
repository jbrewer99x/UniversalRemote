#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
using String = std::string;
using std::max;
constexpr int HIGH = 1, LOW = 0, OUTPUT = 1;
inline uint32_t testMillis = 0;
inline int testPins[64]{};
inline uint32_t millis() { return testMillis; }
inline uint32_t micros() { return testMillis * 1000; }
inline void delay(unsigned ms) { testMillis += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) { testPins[pin] = value; }
inline void ledcSetup(int, int, int) {}
inline void ledcAttachPin(int, int) {}
inline void ledcWrite(int, int) {}
template<typename T, typename L, typename H> T constrain(T value, L lo, H hi) {
    return value < lo ? lo : value > hi ? hi : value;
}
inline long map(long v, long lo, long hi, long outLo, long outHi) {
    return (v - lo) * (outHi - outLo) / (hi - lo) + outLo;
}
struct TestSerial {
    void println(const char* s) { puts(s); }
    template<typename... Args> void printf(const char* format, Args... args) { std::printf(format, args...); }
};
inline TestSerial Serial;
struct TestEsp { unsigned getFreeHeap() { return 100000; } };
inline TestEsp ESP;
