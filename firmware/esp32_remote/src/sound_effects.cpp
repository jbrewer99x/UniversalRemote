#include <Arduino.h>
#include "sound_effects.h"
#include "audio_player.h"

bool playSoundEffect(SoundEffect effect) {
    const char* path = nullptr;

    switch (effect) {
        case SoundEffect::Startup:
            path = "/content/starting-up.wav";
            break;

        case SoundEffect::Waking:
            path = "/content/waking-up.wav";
            break;

        case SoundEffect::UpdateStarting:
            path = "/content/i-have-some-new-tricks.wav";
            break;

        case SoundEffect::Success:
            path = "/content/hell-yeah-brother.wav";
            break;

        case SoundEffect::PcSelected:
            path = "/content/pc.wav";
            break;

        case SoundEffect::RokuSelected:
            path = "/content/roku.wav";
            break;

        case SoundEffect::Battery30:
            path = "/content/im-getting-really-sleepy.wav";
            break;

        case SoundEffect::Battery10:
            path = "/content/im-shutting-down.wav";
            break;

        case SoundEffect::Battery20:
            path = "/content/my-battery-is-dangerously-low.wav";
            break;

        case SoundEffect::BloodyHell:
            path = "/content/oh-bloody-hell.wav";
            break;

        case SoundEffect::PleaseStop:
            path = "/content/oh-bloody-hell-can-you-please-stop.wav";
            break;

        case SoundEffect::Supercalifragilistic:
            path = "/content/supercalafragalisticexbealladocious.wav";
            break;

        case SoundEffect::NewFilesLoaded:
            path = "/content/new-files-loaded.wav";
            break;

        case SoundEffect::Sleeping:
            path = "/content/going-to-sleep.wav";
            break;
    }

    if (path == nullptr) {
        return false;
    }

    return playWav(path);
}
namespace {
bool errorPending = false;
bool errorPlayed = false;
uint32_t lastErrorSound = 0;
}
void reportErrorSound() {
    if (!errorPlayed || millis() - lastErrorSound >= 30000) errorPending = true;
}
void serviceErrorSound() {
    if (!errorPending || isAudioPlaying()) return;
    errorPending = false;
    errorPlayed = true;
    lastErrorSound = millis(); // Set before playback: a missing WAV must not recurse.
    playSoundEffect(SoundEffect::BloodyHell);
}
