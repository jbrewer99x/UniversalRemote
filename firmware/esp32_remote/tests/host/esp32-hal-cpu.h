#pragma once
#include <stdint.h>
inline uint32_t testCpuMhz = 240, testRejectMhz = 0, testClockChanges = 0;
inline bool setCpuFrequencyMhz(uint32_t mhz) {
    if (mhz == testRejectMhz) return false;
    testCpuMhz = mhz;
    ++testClockChanges;
    return true;
}
inline uint32_t getCpuFrequencyMhz() { return testCpuMhz; }
inline uint32_t getApbFrequency() { return 80000000; }
