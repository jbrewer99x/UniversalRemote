#pragma once
#include <stdint.h>
#include <string.h>
#include "config.h"

namespace RemoteNetwork {
constexpr uint32_t AWAKE_POLL_MS = 5000;
constexpr uint32_t SCREEN_OFF_POLL_MS = RemoteConfig::SCREEN_OFF_POLL_INTERVAL_MS;
static_assert(SCREEN_OFF_POLL_MS >= AWAKE_POLL_MS && SCREEN_OFF_POLL_MS <= 3600000,
              "Screen-off polling must be between 5 seconds and 1 hour");
enum class Submit { Accepted, Offline, Full, Paused, Invalid };
struct Command {
    char device[8]{};
    char command[32]{};
    uint32_t queuedAt = 0;
};
enum class Work { None, Command, Expired, Poll };
// Protected by the worker lock. No allocation, I/O, or POST retries.
class Scheduler {
public:
    Submit submit(const char* device, const char* command, uint32_t now, bool online) {
        if (paused) return Submit::Paused;
        if (!online) return Submit::Offline;
        if (!device || !command || !*device || !*command ||
            strlen(device) >= sizeof(Command::device) || strlen(command) >= sizeof(Command::command))
            return Submit::Invalid;
        if (count == 8) return Submit::Full;
        auto &item = commands[(head + count) % 8];
        strcpy(item.device, device);
        strcpy(item.command, command);
        item.queuedAt = now;
        ++count;
        return Submit::Accepted;
    }
    Work begin(uint32_t now, bool online, Command &item) {
        if (paused || active) return Work::None;
        if (count) {
            item = commands[head];
            head = (head + 1) % 8;
            --count;
            active = true;
            return uint32_t(now - item.queuedAt) >= 3000 ? Work::Expired : Work::Command;
        }
        if (online && polling && uint32_t(now - lastPoll) >= effectivePollDelay()) {
            active = true;
            return Work::Poll;
        }
        return Work::None;
    }
    void finish(Work work, bool success, uint32_t now) {
        active = false;
        if (work == Work::Poll) {
            lastPoll = now;
            if (success) { failures = 0; pollDelay = AWAKE_POLL_MS; }
            else {
                if (failures < 5) ++failures;
                pollDelay = 1000U << (failures - 1);
                if (pollDelay > 10000) pollDelay = 10000;
            }
        }
    }
    void pause() { paused = true; head = count = 0; }
    void resume(uint32_t now) { paused = false; lastPoll = now; }
    bool idle() const { return !active; }
    bool isPaused() const { return paused; }
    bool allowPolling(bool enabled) {
        const bool changed = polling != enabled;
        polling = enabled;
        return changed;
    }
    bool setScreenOff(bool off, uint32_t now) {
        if (off == screenOff) return false;
        screenOff = off;
        // Sleep starts a fresh slow interval. Wake requests a fresh poll promptly.
        lastPoll = off ? now : now - effectivePollDelay();
        return true;
    }
    uint32_t waitMs(uint32_t now, bool online) const {
        if (paused) return SCREEN_OFF_POLL_MS;
        if (count) return 0;
        if (!online) return 1000;
        if (!polling) return SCREEN_OFF_POLL_MS;
        const uint32_t elapsed = now - lastPoll;
        return elapsed >= effectivePollDelay() ? 0 : effectivePollDelay() - elapsed;
    }
private:
    Command commands[8];
    uint8_t head = 0, count = 0, failures = 0;
    bool active = false, paused = false, polling = true, screenOff = false;
    uint32_t lastPoll = 0, pollDelay = AWAKE_POLL_MS;
    uint32_t effectivePollDelay() const { return screenOff ? SCREEN_OFF_POLL_MS : pollDelay; }
};
}
