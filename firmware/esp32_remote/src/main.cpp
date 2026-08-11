#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include "config.h"
#include "ota_client.h"
#include "secrets.h"
#include "display.h"
#include "touch.h"
#include "remote_api.h"


Preferences prefs;
bool autoUpdate = RemoteConfig::DEFAULT_AUTO_UPDATE;
bool wasTouching = false;
uint32_t lastWifiAttempt = 0;
uint32_t lastUpdateCheck = 0;
uint8_t uiBrightness = 75;
uint16_t uiSleepSeconds = 30;

#define LCD_DC   41
#define LCD_CS   42
#define LCD_SCK  40
#define LCD_MOSI 45
#define LCD_RST  39
#define LCD_BL   5
#define PWR_KEY_PIN      6
#define PWR_CONTROL_PIN  7
#define BAT_ADC_PIN 8

enum class ScreenMode {
    Home,
    Settings
};

ScreenMode currentScreen = ScreenMode::Home;

enum class RemoteDevice {
    PC,
    Roku
};

RemoteDevice selectedDevice =
    RemoteDevice::PC;

const char* selectedDeviceName() {
    return selectedDevice == RemoteDevice::PC
        ? "pc"
        : "roku";
}

bool connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < RemoteConfig::WIFI_CONNECT_TIMEOUT_MS) {
        delay(300); Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi connection failed");
        return false;
    }

    Serial.printf("Connected. IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

void printInfo() {
    Serial.printf("Firmware: %s\n", RemoteConfig::FIRMWARE_VERSION);
    Serial.printf("Auto update: %s\n", autoUpdate ? "ON" : "OFF");
    Serial.printf("Wi-Fi: %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
    Serial.printf("PSRAM: %u total / %u free\n",
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("Heap free: %u\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("TrueNAS: %s\n", REMOTE_SERVER_URL);
}

float readBatteryVoltage() {
    uint32_t totalMv = 0;

    // Average a few readings to keep the display stable.
    for (int i = 0; i < 8; i++) {
        totalMv += analogReadMilliVolts(BAT_ADC_PIN);
        delay(2);
    }

    float adcVolts = (totalMv / 8.0f) / 1000.0f;

    // Waveshare V2 battery-divider calibration.
    return (adcVolts * 3.0f) / 0.990476f;
}

uint8_t batteryVoltageToPercent(float volts) {
    // Approximation for a normal 1-cell Li-ion/LiPo discharge curve.
    if (volts >= 4.20f) return 100;
    if (volts >= 4.10f) return 90;
    if (volts >= 4.00f) return 80;
    if (volts >= 3.90f) return 70;
    if (volts >= 3.80f) return 55;
    if (volts >= 3.70f) return 40;
    if (volts >= 3.60f) return 25;
    if (volts >= 3.50f) return 15;
    if (volts >= 3.40f) return 8;
    if (volts >= 3.30f) return 3;

    return 0;
}

uint8_t readBatteryPercent() {
    return batteryVoltageToPercent(
        readBatteryVoltage()
    );
}

void checkUpdate(bool install) {
    auto result = OtaClient::check(install);
    Serial.print("OTA: ");
    Serial.println(result.message);
}

void serialConsole() {
    if (!Serial.available()) return;
    char c = Serial.read();

    if (c == 'i') printInfo();
    else if (c == 'c') checkUpdate(false);
    else if (c == 'u') checkUpdate(true);
    else if (c == 'a') {
        autoUpdate = !autoUpdate;
        prefs.putBool("auto_update", autoUpdate);
        Serial.printf("Auto update: %s\n", autoUpdate ? "ON" : "OFF");
    } else if (c == 'r') {
        ESP.restart();
    } else if (c == 'h' || c == '?') {
        Serial.println("h help | i info | c check | u update | a auto-update toggle | r reboot");
    }
}

void setup() {
      // Latch battery power ON
    pinMode(PWR_CONTROL_PIN, OUTPUT);
    digitalWrite(PWR_CONTROL_PIN, HIGH);
    Serial.begin(115200);
    delay(1000);
    Serial.printf(
    "\nUniversal Remote ESP32 Bootstrap %s\n",
    RemoteConfig::FIRMWARE_VERSION
);
    Serial.println("Target: Waveshare ESP32-S3-Touch-LCD-2.8");
    initDisplay();
    setDisplayBrightness(uiBrightness);
    initTouch();
    prefs.begin("remote", false);
    uiBrightness = prefs.getUChar(
    "brightness",
    75
);
setDisplayBrightness(uiBrightness);

uiSleepSeconds = prefs.getUShort(
    "sleep_sec",
    30
);
    autoUpdate = prefs.getBool("auto_update", RemoteConfig::DEFAULT_AUTO_UPDATE);

    OtaClient::begin();
    connectWifi();
    printInfo();

    displayStatus(
    WiFi.status() == WL_CONNECTED,
    WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString()
        : "0.0.0.0",
    "OTA OK"
    );
// Battery percentage
updateBatteryStatus(readBatteryPercent());
    // Wait 60 seconds before the first automatic update.
    lastUpdateCheck = millis();
}

void loop() {
    serialConsole();
   RemoteTouchPoint point = readTouch();

bool newTap = point.touched && !wasTouching;

if (newTap) {

    if (
    currentScreen == ScreenMode::Home &&
    point.touched
) {

    // Status bar -> Settings
    if (point.y < 28) {
        currentScreen = ScreenMode::Settings;

        displaySettings(
            uiBrightness,
            uiSleepSeconds
        );

        updateBatteryStatus(
            readBatteryPercent()
        );

        return;
    }

    // PC
    if (
        point.x >= 8 &&
        point.x <= 116 &&
        point.y >= 36 &&
        point.y <= 66
    ) {
        selectedDevice =
            RemoteDevice::PC;

        displayStatus(
            WiFi.status() == WL_CONNECTED,
            WiFi.localIP().toString(),
            "Ready"
        );

        updateBatteryStatus(
            readBatteryPercent()
        );

        return;
    }

    // Roku
    if (
        point.x >= 124 &&
        point.x <= 232 &&
        point.y >= 36 &&
        point.y <= 66
    ) {
        selectedDevice =
            RemoteDevice::Roku;

        displayStatus(
            WiFi.status() == WL_CONNECTED,
            WiFi.localIP().toString(),
            "Ready"
        );

        updateBatteryStatus(
            readBatteryPercent()
        );

        return;
    }

    const char *device =
        selectedDeviceName();

    // Power
    if (point.x >= 8 && point.x <= 78 &&
        point.y >= 74 && point.y <= 106) {
        sendRemoteCommand(device, "power");
    }

    // Home
    else if (point.x >= 85 && point.x <= 155 &&
             point.y >= 74 && point.y <= 106) {
        sendRemoteCommand(device, "home");
    }

    // Back
    else if (point.x >= 162 && point.x <= 232 &&
             point.y >= 74 && point.y <= 106) {
        sendRemoteCommand(device, "back");
    }

    // Up
    else if (point.x >= 88 && point.x <= 152 &&
             point.y >= 113 && point.y <= 147) {
        sendRemoteCommand(device, "up");
    }

    // Left
    else if (point.x >= 17 && point.x <= 81 &&
             point.y >= 151 && point.y <= 189) {
        sendRemoteCommand(device, "left");
    }

    // OK
    else if (point.x >= 88 && point.x <= 152 &&
             point.y >= 151 && point.y <= 189) {
        sendRemoteCommand(device, "ok");
    }

    // Right
    else if (point.x >= 159 && point.x <= 223 &&
             point.y >= 151 && point.y <= 189) {
        sendRemoteCommand(device, "right");
    }

    // Down
    else if (point.x >= 88 && point.x <= 152 &&
             point.y >= 193 && point.y <= 227) {
        sendRemoteCommand(device, "down");
    }

    // Previous
    else if (point.x >= 8 && point.x <= 78 &&
             point.y >= 235 && point.y <= 267) {
        sendRemoteCommand(device, "previous");
    }

    // Play/Pause
    else if (point.x >= 85 && point.x <= 155 &&
             point.y >= 235 && point.y <= 267) {
        sendRemoteCommand(device, "play_pause");
    }

    // Next
    else if (point.x >= 162 && point.x <= 232 &&
             point.y >= 235 && point.y <= 267) {
        sendRemoteCommand(device, "next");
    }

    // Volume -
    else if (point.x >= 8 && point.x <= 78 &&
             point.y >= 275 && point.y <= 311) {
        sendRemoteCommand(device, "volume_down");
    }

    // Mute
    else if (point.x >= 85 && point.x <= 155 &&
             point.y >= 275 && point.y <= 311) {
        sendRemoteCommand(device, "mute");
    }

    // Volume +
    else if (point.x >= 162 && point.x <= 232 &&
             point.y >= 275 && point.y <= 311) {
        sendRemoteCommand(device, "volume_up");
    }

}
    wasTouching = point.touched;
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED &&
        now - lastWifiAttempt >= RemoteConfig::WIFI_RETRY_INTERVAL_MS) {
        lastWifiAttempt = now;
        connectWifi();
    }

    if (autoUpdate &&
        WiFi.status() == WL_CONNECTED &&
        now - lastUpdateCheck >= RemoteConfig::OTA_CHECK_INTERVAL_MS) {
        lastUpdateCheck = now;
        checkUpdate(true);
    }

    delay(5);
}
