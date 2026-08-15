#pragma once

#include <stdint.h>

constexpr uint8_t DEFAULT_VOLUME = 12;
constexpr uint8_t MAX_VOLUME = 21;

bool initAudio();
bool playWav(const char* path);
void serviceAudio();
void stopAudio();
void setAudioVolume(uint8_t volume);
bool isAudioPlaying();