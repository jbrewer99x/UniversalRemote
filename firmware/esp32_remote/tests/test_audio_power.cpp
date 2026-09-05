#include <cassert>
#include "Arduino.h"
#include "Audio.h"
#include "esp32-hal-cpu.h"
#include "audio_player.h"
#include "power_manager.h"

static unsigned errors = 0;
bool isSdCardReady() { return true; }
void reportErrorSound() { ++errors; }

int main() {
    Power::begin();
    Power::setScreenOff(true);
    assert(initAudio());
    assert(!testI2SRunning && !isAudioPlaying() && testCpuMhz == 80);
    testDecodeLoops = 1;
    assert(playWav("/content/findme.wav"));
    assert(testI2SRunning && isAudioPlaying() && testCpuMhz == 160);
    serviceAudio(); // EOF, but samples are still buffered for the speaker
    assert(!testDecoderRunning && isAudioPlaying() && testI2SRunning);
    delay(190); serviceAudio(); assert(isAudioPlaying());
    delay(1); serviceAudio();
    assert(!isAudioPlaying() && !testI2SRunning && testCpuMhz == 80);
    assert(playWav("/content/findme.wav")); // repeat Find Remote restarts the output
    assert(testI2SRunning && testCpuMhz == 160);
    stopAudio(); // pickup cancels immediately, including a queued tail
    assert(!isAudioPlaying() && !testI2SRunning && testCpuMhz == 80);
    testOpenFailure = true;
    assert(!playWav("/content/findme.wav"));
    assert(!isAudioPlaying() && !testI2SRunning && testCpuMhz == 80);
    testOpenFailure = false;
    testI2SStartFailure = true;
    assert(!playWav("/content/findme.wav")); assert(testCpuMhz == 80);
    testI2SStartFailure = false;
    testDecodeLoops = 1;
    assert(playWav("/content/startup.wav"));
    const auto started = millis();
    finishAudioPlayback();
    assert(millis() - started >= 191);
    assert(!testI2SRunning && !isAudioPlaying());
    assert(errors == 2);
    puts("PASS: idle audio clocks, screen-off Find Remote wake, DMA tail, pickup, restart/failures");
}
