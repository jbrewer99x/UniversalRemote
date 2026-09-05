#pragma once

enum class SoundEffect {
    Startup,
    Waking,
    UpdateStarting,
    NewFilesLoaded,
    Success,

    PcSelected,
    RokuSelected,

    Battery30,
    Battery20,
    Battery10,

    BloodyHell,
    PleaseStop,
    Sleeping,
    Supercalifragilistic
};

bool playSoundEffect(SoundEffect effect);
// Coalesce errors and defer playback until normal audio is idle.
void reportErrorSound();
void serviceErrorSound();