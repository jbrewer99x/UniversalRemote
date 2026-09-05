#include "sound_effects.h"
#include <Arduino.h>
#include <SD.h>
#include <Audio.h>

#include "audio_player.h"
#include "sd_storage.h"
#include "power_manager.h"
#include "power_policy.h"

#define I2S_DOUT 47
#define I2S_BCLK 48
#define I2S_LRC  38

static Audio audio;
static bool audioReady = false;
static bool outputClockRunning = true; // Audio constructor installs/starts I2S.
static Power::AudioTail tail;

bool initAudio() {
    Serial.println("Audio: initializing");

    if (!audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT)) {
        Serial.println("Audio: pin configuration failed");
        reportErrorSound();
        return false;
    }

    audio.setVolume(
        DEFAULT_VOLUME
    );

    audioReady = true;
    stopAudio(); // Idle DMA/I2S need not keep the amplifier clocked.
    Serial.println("Audio: ready (output clocks stopped)");

    return true;
}

bool playWav(const char* path) {
    if (!audioReady) return false;
    if (!isSdCardReady()) {
        Serial.println(
            "Audio: SD card unavailable"
        );

        reportErrorSound();
        return false;
    }

    if (!SD.exists(path)) {
        Serial.printf(
            "Audio: file not found: %s\n",
            path
        );

        reportErrorSound();
        return false;
    }

    Power::setAudioActive(true);
    tail.clear();
    if (!outputClockRunning) {
        if (i2s_start(I2S_NUM_0) != ESP_OK) {
            Power::setAudioActive(false);
            reportErrorSound();
            return false;
        }
        outputClockRunning = true;
    }
    Serial.printf(
        "Audio: playing %s\n",
        path
    );

    bool ok = audio.connecttoFS(
        SD,
        path
    );

    if (!ok) {
        stopAudio();
        Serial.println(
            "Audio: failed to open file"
        );

        reportErrorSound();
        return false;
    }

    return true;
}

void serviceAudio() {
    if (!audioReady || !outputClockRunning) return;
    audio.loop();
    if (audio.isRunning()) tail.clear();
    else {
        if (!tail.started()) tail.start(millis(), audio.getSampleRate());
        if (!tail.active(millis())) stopAudio();
    }
}

void setAudioVolume(uint8_t volume) {
    if (volume > MAX_VOLUME) {
        volume = MAX_VOLUME;
    }

    audio.setVolume(volume);

    Serial.printf(
        "Audio: volume = %u\n",
        volume
    );
}

bool isAudioPlaying() {
    return audioReady && (audio.isRunning() || tail.started());
}
void stopAudio() {
    if (!audioReady) return;
    // stopSong clears decoder buffers even after isRunning() becomes false.
    audio.stopSong();
#if ESP_IDF_VERSION_MAJOR < 5
    i2s_zero_dma_buffer(I2S_NUM_0);
#endif
    tail.clear();
    if (outputClockRunning) {
        if (i2s_stop(I2S_NUM_0) == ESP_OK) outputClockRunning = false;
        else Serial.println("Audio: failed to stop I2S output clock");
    }
    Power::setAudioActive(outputClockRunning);
}

void finishAudioPlayback() {
    const uint32_t started = millis();
    while (isAudioPlaying() && millis() - started < 15000) {
        serviceAudio();
        delay(1);
    }
    if (isAudioPlaying()) reportErrorSound();
    // serviceAudio keeps isAudioPlaying true until the DMA tail has drained.
    stopAudio();
}
