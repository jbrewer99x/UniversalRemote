#!/usr/bin/env python3
from pathlib import Path
import sys

path = Path("firmware/esp32_remote/src/main.cpp")
if not path.exists():
    sys.exit(f"ERROR: {path} not found. Run this from the UniversalRemote repo root.")

text = path.read_text(encoding="utf-8")
original = text

def replace_once(old, new, label):
    global text
    if new in text:
        print(f"SKIP: {label} already present")
        return
    if old not in text:
        sys.exit(f"ERROR: Could not find anchor for: {label}\nNo changes were written.")
    text = text.replace(old, new, 1)
    print(f"OK: {label}")

replace_once(
    '#include <WiFi.h>\n',
    '#include <WiFi.h>\n#include <esp_sleep.h>\n#include <driver/gpio.h>\n',
    "ESP32 sleep includes",
)

replace_once(
    '#define BAT_ADC_PIN      8\n',
    '#define BAT_ADC_PIN      8\n#define TOUCH_WAKE_PIN   4\n',
    "touch wake pin",
)

replace_once(
    'static constexpr uint32_t IMU_CHECK_INTERVAL_MS = 100;\n',
    '// 4 Hz pickup polling while the screen is off.\nstatic constexpr uint32_t IMU_CHECK_INTERVAL_MS = 250;\n',
    "slower screen-off IMU polling",
)

replace_once(
    'static constexpr float IMU_WAKE_THRESHOLD_G = 0.12f;\n',
    'static constexpr float IMU_WAKE_THRESHOLD_G = 0.12f;\n// Preserve screen-off + IMU wake for one hour before true light sleep.\nstatic constexpr uint32_t LIGHT_SLEEP_AFTER_MS = 60UL * 60UL * 1000UL;\n',
    "60-minute light-sleep threshold",
)

light_sleep_fn = '''
void enterLightSleep() {
    if (!screenSleeping) return;

    if (findRemoteActive || soundTestActive || isAudioPlaying()) {
        return;
    }

    Serial.println("Sleep: 60 minutes idle; entering ESP32 light sleep");

    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    delay(30);

    pinMode(TOUCH_WAKE_PIN, INPUT_PULLUP);
    gpio_wakeup_enable(
        (gpio_num_t)TOUCH_WAKE_PIN,
        GPIO_INTR_LOW_LEVEL
    );
    esp_sleep_enable_gpio_wakeup();

    Serial.println("Sleep: light sleep armed; touch screen to wake");
    Serial.flush();

    esp_err_t sleepResult = esp_light_sleep_start();

    gpio_wakeup_disable((gpio_num_t)TOUCH_WAKE_PIN);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    if (sleepResult != ESP_OK) {
        Serial.printf(
            "Sleep: esp_light_sleep_start failed: %d\n",
            (int)sleepResult
        );
    } else {
        Serial.println("Sleep: woke from light sleep");
    }

    lastWifiAttempt = 0;
    connectWifi();
    wakeScreen("touch/light sleep");
}

'''

if "void enterLightSleep()" not in text:
    anchor = "bool imuWakeMotionDetected() {\n"
    if anchor not in text:
        sys.exit("ERROR: Could not find imuWakeMotionDetected() anchor.\nNo changes were written.")
    text = text.replace(anchor, light_sleep_fn + anchor, 1)
    print("OK: light-sleep function")
else:
    print("SKIP: light-sleep function already present")

old_sleep_branch = '''        if (!point.touched) {
            wasTouching = false;
        }
    } else {
'''

new_sleep_branch = '''        if (!point.touched) {
            wasTouching = false;
        }

        // After a full hour without activity, enter true light sleep.
        if (screenSleeping && now - lastActivityAt >= LIGHT_SLEEP_AFTER_MS) {
            enterLightSleep();
            now = millis();
        }
    } else {
'''

if "now - lastActivityAt >= LIGHT_SLEEP_AFTER_MS" not in text:
    if old_sleep_branch not in text:
        sys.exit("ERROR: Could not find screenSleeping branch anchor.\nNo changes were written.")
    text = text.replace(old_sleep_branch, new_sleep_branch, 1)
    print("OK: 60-minute light-sleep transition")
else:
    print("SKIP: light-sleep transition already present")

replace_once(
    '    delay(5);\n',
    '    // Slower loop while the display is dark.\n    delay(screenSleeping ? 20 : 5);\n',
    "slower screen-off main loop",
)

if text == original:
    print("No changes needed.")
    sys.exit(0)

backup = path.with_suffix(".cpp.before-light-sleep")
if not backup.exists():
    backup.write_text(original, encoding="utf-8")
    print(f"Backup: {backup}")

path.write_text(text, encoding="utf-8")
print(f"UPDATED: {path}")
print("Next: git diff -- firmware/esp32_remote/src/main.cpp")
