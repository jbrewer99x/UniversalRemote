#pragma once
#include <stdint.h>

namespace Power {
// Keep the PLL/APB operating range while Wi-Fi, USB, SPI and I2S are available.
constexpr uint32_t IDLE_MHZ = 80, ACTIVE_MHZ = 160, MAX_MHZ = 240;
constexpr uint32_t UI_HOLD_MS = 120, PRESSURE_HOLD_MS = 250;
constexpr uint32_t RENDER_CPU_BUDGET_US = 8000;

class Policy {
public:
    void uiActivity(uint32_t now) { if (!screenOff) { uiAt = now; uiPending = true; } }
    void renderCost(uint32_t cpuUs, uint32_t now) {
        if (!screenOff && cpuUs > RENDER_CPU_BUDGET_US) { pressureAt = now; pressurePending = true; }
    }
    void setScreenOff(bool off) {
        screenOff = off;
        if (off) uiPending = pressurePending = false;
    }
    void setAudio(bool active) { audio = active; }
    void beginHeavy() { ++heavy; }
    void endHeavy() { if (heavy) --heavy; }
    uint32_t target(uint32_t now) {
        if (uiPending && uint32_t(now - uiAt) >= UI_HOLD_MS) uiPending = false;
        if (pressurePending && uint32_t(now - pressureAt) >= PRESSURE_HOLD_MS) pressurePending = false;
        if (heavy || pressurePending) return MAX_MHZ;
        if (audio || uiPending) return ACTIVE_MHZ;
        return IDLE_MHZ;
    }
private:
    bool screenOff = false, audio = false, uiPending = false, pressurePending = false;
    uint16_t heavy = 0;
    uint32_t uiAt = 0, pressureAt = 0;
};

// Decoder EOF can precede the last audible DMA frame by over a second.
class AudioTail {
public:
    void start(uint32_t now, uint32_t rate) {
        startedAt = now;
        duration = rate >= 8000 ? (16UL * 512UL * 1000UL + rate - 1) / rate + 20 : 1050;
        pending = true;
    }
    bool active(uint32_t now) const { return pending && uint32_t(now - startedAt) < duration; }
    bool started() const { return pending; }
    void clear() { pending = false; }
private:
    bool pending = false;
    uint32_t startedAt = 0, duration = 0;
};
}
