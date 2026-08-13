#pragma once

bool initAudio();
bool playWav(const char* path);
void serviceAudio();
void setAudioVolume(uint8_t volume);
bool isAudioPlaying();