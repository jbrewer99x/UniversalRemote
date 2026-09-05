#pragma once
#include <stdint.h>

// Main-loop owned: never call these from a worker, ISR, or inside an SPI transaction.
namespace Power {
void begin();
void service();
void uiActivity();
void observeRender(uint32_t cpuUs);
void setAudioActive(bool active);
void setScreenOff(bool off);
void beforeLightSleep();
void afterLightSleep();
void printStats();
void beginHeavyWork();
void endHeavyWork();
class HeavyWork {
public:
    HeavyWork() { beginHeavyWork(); }
    ~HeavyWork() { endHeavyWork(); }
    HeavyWork(const HeavyWork&) = delete;
    HeavyWork& operator=(const HeavyWork&) = delete;
};
}
