#include <cassert>
#include "Arduino.h"
#include "esp32-hal-cpu.h"
#include "esp32-hal-bt.h"
#include "power_policy.h"
#include "power_manager.h"

extern "C" bool btInUse();

int main(int argc, char**) {
    if (argc > 1) {
        testRejectMhz = 80;
        Power::begin();
        assert(testCpuMhz == 240); // safe fallback, not a failed low clock
        const auto changes = testClockChanges;
        delay(10000); Power::service(); Power::uiActivity();
        assert(testClockChanges == changes); // no retry storm
        puts("PASS: clock failure falls back and disables further switching");
        return 0;
    }
    Power::begin();
    assert(!btInUse() && !btStarted());
    assert(testCpuMhz == 80);
    Power::uiActivity(); assert(testCpuMhz == 160);
    delay(Power::UI_HOLD_MS - 1); Power::service(); assert(testCpuMhz == 160);
    delay(1); Power::service(); assert(testCpuMhz == 80);
    Power::observeRender(1000); assert(testCpuMhz == 80);
    Power::observeRender(Power::RENDER_CPU_BUDGET_US + 1); assert(testCpuMhz == 240);
    delay(Power::PRESSURE_HOLD_MS); Power::service(); assert(testCpuMhz == 80);
    Power::setScreenOff(true);
    Power::uiActivity(); Power::observeRender(50000); assert(testCpuMhz == 80);
    Power::setAudioActive(true); assert(testCpuMhz == 160); // Find Remote with screen off
    {
        Power::HeavyWork outer;
        assert(testCpuMhz == 240);
        { Power::HeavyWork inner; assert(testCpuMhz == 240); }
        assert(testCpuMhz == 240);
    }
    assert(testCpuMhz == 160); // a nested boost cannot clear audio's requirement
    Power::setAudioActive(false); assert(testCpuMhz == 80);
    Power::beforeLightSleep(); delay(3600000); Power::afterLightSleep();
    Power::setScreenOff(false);
    testMillis = UINT32_MAX - 30;
    Power::uiActivity(); assert(testCpuMhz == 160);
    delay(Power::UI_HOLD_MS); Power::service(); assert(testCpuMhz == 80);
    Power::AudioTail tail;
    tail.start(UINT32_MAX - 10, 8000);
    assert(tail.active(500));
    assert(!tail.active(1033)); // wrap-safe 1024 ms DMA plus 20 ms margin
    tail.clear(); assert(!tail.started());
    puts("PASS: clock tiers, hysteresis, screen-off audio, nested boosts, Bluetooth off, rollover");
}
