#pragma once
#include <stdint.h>

// Hardware-independent state, shared by firmware and host tests.
class TouchState {
public:
    enum class Sample { None, Down, Up };
    bool update(Sample sample, uint32_t now) {
        if (sample == Sample::Down) { down = true; releasing = false; }
        if (sample == Sample::Up && down && !releasing) {
            releasing = true;
            releaseAt = now;
        }
        // IRQ-high alone is never a release; filter short empty/error frames.
        if (releasing && uint32_t(now - releaseAt) >= 25) down = releasing = false;
        return down;
    }
private:
    bool down = false, releasing = false;
    uint32_t releaseAt = 0;
};
class WakeTouchGate {
public:
    void suppress() { blocked = true; }
    bool accept(bool down) {
        if (!down) blocked = false;
        return down && !blocked;
    }
private:
    bool blocked = false;
};
template <typename T> struct SavedSetting {
    T saved{};
    bool needsSave(T value) const { return value != saved; }
    void committed(T value, bool success) { if (success) saved = value; }
};
