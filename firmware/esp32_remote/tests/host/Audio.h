#pragma once
#include "SD.h"
#include <stdint.h>
#define ESP_IDF_VERSION_MAJOR 4
constexpr int ESP_OK = 0, I2S_NUM_0 = 0;
inline bool testI2SRunning = true, testI2SStartFailure = false;
inline bool testDecoderRunning = false, testOpenFailure = false;
inline unsigned testDecodeLoops = 0;
inline uint32_t testAudioRate = 48000;
inline int i2s_start(int) { if (testI2SStartFailure) return -1; testI2SRunning = true; return ESP_OK; }
inline int i2s_stop(int) { testI2SRunning = false; return ESP_OK; }
inline void i2s_zero_dma_buffer(int) {}
class Audio {
public:
    bool setPinout(int, int, int) { return true; }
    void setVolume(uint8_t) {}
    bool connecttoFS(TestSD&, const char*) { testDecoderRunning = !testOpenFailure; return testDecoderRunning; }
    void loop() { if (testDecoderRunning && testDecodeLoops && !--testDecodeLoops) testDecoderRunning = false; }
    bool isRunning() { return testDecoderRunning; }
    void stopSong() { testDecoderRunning = false; }
    uint32_t getSampleRate() { return testAudioRate; }
};
