#pragma once

enum class SoundEffect {
    Startup,
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

    Supercalifragilistic
};

bool playSoundEffect(SoundEffect effect);