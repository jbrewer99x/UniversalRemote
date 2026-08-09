#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include "config.h"
#include "ota_client.h"
#include "secrets.h"

Preferences prefs;
bool autoUpdate = RemoteConfig::DEFAULT_AUTO_UPDATE;
uint32_t lastWifiAttempt = 0;
uint32_t lastUpdateCheck = 0;

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
    Serial.begin(115200);
    delay(1000);
    Serial.println("\nUniversal Remote ESP32 Bootstrap 0.1.0");
    Serial.println("Target: Waveshare ESP32-S3-Touch-LCD-2.8");

    prefs.begin("remote", false);
    autoUpdate = prefs.getBool("auto_update", RemoteConfig::DEFAULT_AUTO_UPDATE);

    OtaClient::begin();
    connectWifi();
    printInfo();

    // Wait 60 seconds before the first automatic update.
    lastUpdateCheck = millis();
}

void loop() {
    serialConsole();
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
