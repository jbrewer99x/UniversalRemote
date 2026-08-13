#include <Arduino.h>
#include <SD.h>
#include <Audio.h>

#include "audio_player.h"
#include "sd_storage.h"

#define I2S_DOUT 47
#define I2S_BCLK 48
#define I2S_LRC  38

static Audio audio;

static constexpr uint8_t DEFAULT_VOLUME = 12;
static constexpr uint8_t MAX_VOLUME = 21;

bool initAudio() {
    Serial.println("Audio: initializing");

    audio.setPinout(
        I2S_BCLK,
        I2S_LRC,
        I2S_DOUT
    );

    audio.setVolume(
        DEFAULT_VOLUME
    );

    Serial.println("Audio: ready");

    return true;
}

bool playWav(const char* path) {
    if (!isSdCardReady()) {
        Serial.println(
            "Audio: SD card unavailable"
        );

        return false;
    }

    if (!SD.exists(path)) {
        Serial.printf(
            "Audio: file not found: %s\n",
            path
        );

        return false;
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
        Serial.println(
            "Audio: failed to open file"
        );

        return false;
    }

    return true;
}

void serviceAudio() {
    audio.loop();
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
    return audio.isRunning();
}