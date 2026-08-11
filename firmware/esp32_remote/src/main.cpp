#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include "config.h"
#include "ota_client.h"
#include "secrets.h"
#include "display.h"
#include "touch.h"

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

enum class ScreenMode {
    Home,
    Settings
};

ScreenMode currentScreen = ScreenMode::Home;


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

    // Wait 60 seconds before the first automatic update.
    lastUpdateCheck = millis();
}

void loop() {
    serialConsole();
   RemoteTouchPoint point = readTouch();

bool newTap = point.touched && !wasTouching;

if (newTap) {

    // HOME -> SETTINGS
    if (
        currentScreen == ScreenMode::Home &&
        point.y < 28
    ) {
        currentScreen = ScreenMode::Settings;

        Serial.println("UI: opening Settings");

        displaySettings(
            uiBrightness,
            uiSleepSeconds
        );
    }

    // SETTINGS
    else if (currentScreen == ScreenMode::Settings) {

        // Back button
        if (
            point.y < 36 &&
            point.x < 75
        ) {
            currentScreen = ScreenMode::Home;

            Serial.println("UI: returning Home");

            displayStatus(
                WiFi.status() == WL_CONNECTED,
                WiFi.status() == WL_CONNECTED
                    ? WiFi.localIP().toString()
                    : "0.0.0.0",
                "Ready"
            );
        }

        // Brightness slider
        else if (
            point.y >= 70 &&
            point.y <= 105 &&
            point.x >= 14 &&
            point.x <= 226
        ) {
            int value = map(
                point.x,
                14,
                226,
                0,
                100
            );

            uiBrightness = constrain(value, 0, 100);

        
        setDisplayBrightness(uiBrightness);
        updateBrightnessSlider(uiBrightness);

            Serial.printf(
                "UI: brightness = %u%%\n",
                uiBrightness
            );
        }

        // Sleep timer slider
        else if (
            point.y >= 138 &&
            point.y <= 175 &&
            point.x >= 14 &&
            point.x <= 226
        ) {
            int value = map(
                point.x,
                14,
                226,
                2,
                120
            );

            uiSleepSeconds = constrain(
                value,
                2,
                120
            );

           updateSleepSlider(uiSleepSeconds);

          

            Serial.printf(
                "UI: sleep timer = %u sec\n",
                uiSleepSeconds
            );
        }
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
