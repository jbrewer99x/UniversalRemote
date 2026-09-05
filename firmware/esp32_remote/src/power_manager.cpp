#include <Arduino.h>
#include <esp32-hal-cpu.h>
#include <esp32-hal-bt.h>
#include <esp_bt.h>
#include "power_manager.h"
#include "power_policy.h"

// Arduino's S3 startup uses this hook to release unused Bluetooth controller RAM.
// This product uses Wi-Fi only; Bluetooth is intentionally unavailable until reboot.
extern "C" bool btInUse() { return false; }

namespace Power {
namespace {
Policy policy;
bool ready = false, clockHealthy = true;
uint32_t currentMhz = 240, lastAccounted = 0, transitions = 0, failures = 0;
uint32_t maxRenderCpuUs = 0;
uint64_t residencyMs[3]{};
unsigned bucket(uint32_t mhz) { return mhz <= 80 ? 0 : mhz <= 160 ? 1 : 2; }
void account() {
    const auto now = millis();
    residencyMs[bucket(currentMhz)] += uint32_t(now - lastAccounted);
    lastAccounted = now;
}
}

void begin() {
#ifdef CONFIG_BT_ENABLED
    // Also stop a controller enabled by earlier startup code, without starting it.
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE && !btStop())
        Serial.println("Power: Bluetooth shutdown failed");
#endif
    currentMhz = getCpuFrequencyMhz();
    lastAccounted = millis();
    ready = true;
    service();
    Serial.println("Power: Arduino clock governor, 80/160/240 MHz; Bluetooth unused");
}
void service() {
    if (!ready) return;
    account();
    const auto wanted = policy.target(millis());
    if (!clockHealthy || wanted == currentMhz) return;
    if (setCpuFrequencyMhz(wanted)) {
        currentMhz = getCpuFrequencyMhz();
        ++transitions;
    } else {
        ++failures;
        clockHealthy = false;
        // A clock error must not cause endless retries or leave demanding work underclocked.
        setCpuFrequencyMhz(MAX_MHZ);
        currentMhz = getCpuFrequencyMhz();
        Serial.printf("Power: clock change failed; governor stopped at %lu MHz\n", (unsigned long)currentMhz);
    }
}
void uiActivity() { policy.uiActivity(millis()); service(); }
void observeRender(uint32_t cpuUs) {
    maxRenderCpuUs = max(maxRenderCpuUs, cpuUs);
    policy.renderCost(cpuUs, millis());
    service();
}
void setAudioActive(bool active) { policy.setAudio(active); service(); }
void setScreenOff(bool off) { policy.setScreenOff(off); service(); }
void beginHeavyWork() { policy.beginHeavy(); service(); }
void endHeavyWork() { policy.endHeavy(); service(); }
void beforeLightSleep() { policy.setScreenOff(true); service(); }
void afterLightSleep() {
    // Time spent with CPUs stopped is not 80 MHz residency.
    lastAccounted = millis();
    currentMhz = getCpuFrequencyMhz();
    service();
}
void printStats() {
    if (!ready) return;
    account();
    Serial.printf("Power: CPU=%lu MHz APB=%lu Hz transitions=%lu failures=%lu\n",
                  (unsigned long)currentMhz, (unsigned long)getApbFrequency(),
                  (unsigned long)transitions, (unsigned long)failures);
    Serial.printf("Power: awake residency 80/160/240 MHz=%llu/%llu/%llu ms; max render CPU=%lu us; BT=%s\n",
                  residencyMs[0], residencyMs[1], residencyMs[2], (unsigned long)maxRenderCpuUs,
                  btStarted() ? "ON (unexpected)" : "off");
}
}
