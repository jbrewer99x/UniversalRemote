#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "config.h"
#include "ota_client.h"
#include "secrets.h"
#include "display.h"
#include "touch.h"
#include "remote_api.h"


// -----------------------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------------------

#define PWR_KEY_PIN      6
#define PWR_CONTROL_PIN  7
#define BAT_ADC_PIN      8


// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

Preferences prefs;

bool autoUpdate = RemoteConfig::DEFAULT_AUTO_UPDATE;

uint32_t lastWifiAttempt = 0;
uint32_t lastUpdateCheck = 0;
uint32_t lastBatteryUpdate = 0;

static constexpr uint32_t BATTERY_UPDATE_INTERVAL_MS = 30000;

uint8_t uiBrightness = 75;
uint16_t uiSleepSeconds = 30;

bool wasTouching = false;


enum class ScreenMode {
    Home,
    Settings
};

ScreenMode currentScreen = ScreenMode::Home;


enum class RemoteDevice {
    PC,
    Roku
};

RemoteDevice selectedDevice = RemoteDevice::PC;


// -----------------------------------------------------------------------------
// Device selection
// -----------------------------------------------------------------------------

const char* selectedDeviceName() {
    return selectedDevice == RemoteDevice::PC
        ? "pc"
        : "roku";
}


// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

bool connectWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    Serial.printf("Connecting to %s", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();

    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - start < RemoteConfig::WIFI_CONNECT_TIMEOUT_MS
    ) {
        delay(300);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi connection failed");
        return false;
    }

    Serial.printf(
        "Connected. IP=%s RSSI=%d dBm\n",
        WiFi.localIP().toString().c_str(),
        WiFi.RSSI()
    );

    return true;
}


// -----------------------------------------------------------------------------
// Battery
// -----------------------------------------------------------------------------

float readBatteryVoltage() {
    uint32_t totalMv = 0;

    for (int i = 0; i < 8; i++) {
        totalMv += analogReadMilliVolts(BAT_ADC_PIN);
        delay(2);
    }

    float adcVolts =
        (totalMv / 8.0f) / 1000.0f;

    // Waveshare V2 battery-divider calibration.
    return (adcVolts * 3.0f) / 0.990476f;
}


uint8_t batteryVoltageToPercent(float volts) {
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


void refreshBatteryStatus() {
    updateBatteryStatus(
        readBatteryPercent()
    );

    lastBatteryUpdate = millis();
}


// -----------------------------------------------------------------------------
// Screen drawing
// -----------------------------------------------------------------------------

 void showHome() {
    currentScreen = ScreenMode::Home;

    displayStatus(
        WiFi.status() == WL_CONNECTED,
        WiFi.status() == WL_CONNECTED
            ? WiFi.localIP().toString()
            : "0.0.0.0",
        "Ready",
        selectedDevice == RemoteDevice::PC
    );

    refreshBatteryStatus();
}


void showSettings() {
    currentScreen = ScreenMode::Settings;

    displaySettings(
        uiBrightness,
        uiSleepSeconds
    );

    refreshBatteryStatus();
}


// -----------------------------------------------------------------------------
// OTA / diagnostics
// -----------------------------------------------------------------------------

void printInfo() {
    Serial.printf(
        "Firmware: %s\n",
        RemoteConfig::FIRMWARE_VERSION
    );

    Serial.printf(
        "Auto update: %s\n",
        autoUpdate ? "ON" : "OFF"
    );

    Serial.printf(
        "Wi-Fi: %s\n",
        WiFi.status() == WL_CONNECTED
            ? "connected"
            : "disconnected"
    );

    Serial.printf(
        "PSRAM: %u total / %u free\n",
        (unsigned)ESP.getPsramSize(),
        (unsigned)ESP.getFreePsram()
    );

    Serial.printf(
        "Heap free: %u\n",
        (unsigned)ESP.getFreeHeap()
    );

    Serial.printf(
        "TrueNAS: %s\n",
        REMOTE_SERVER_URL
    );

    Serial.printf(
        "Battery: %.2f V / %u%%\n",
        readBatteryVoltage(),
        readBatteryPercent()
    );

    Serial.printf(
        "Selected device: %s\n",
        selectedDeviceName()
    );
}


void checkUpdate(bool install) {
    auto result = OtaClient::check(install);

    Serial.print("OTA: ");
    Serial.println(result.message);
}


void serialConsole() {
    if (!Serial.available()) {
        return;
    }

    char c = Serial.read();

    if (c == 'i') {
        printInfo();
    }
    else if (c == 'c') {
        checkUpdate(false);
    }
    else if (c == 'u') {
        checkUpdate(true);
    }
    else if (c == 'a') {
        autoUpdate = !autoUpdate;

        prefs.putBool(
            "auto_update",
            autoUpdate
        );

        Serial.printf(
            "Auto update: %s\n",
            autoUpdate ? "ON" : "OFF"
        );
    }
    else if (c == 'r') {
        ESP.restart();
    }
    else if (c == 'h' || c == '?') {
        Serial.println(
            "h help | i info | c check | u update | "
            "a auto-update toggle | r reboot"
        );
    }
}


// -----------------------------------------------------------------------------
// Remote commands
// -----------------------------------------------------------------------------

void sendSelectedCommand(const char* command) {
    const char* device = selectedDeviceName();

    Serial.printf(
        "UI: %s -> %s\n",
        device,
        command
    );

    sendRemoteCommand(
        device,
        command
    );
}


// -----------------------------------------------------------------------------
// Home touch handling
// -----------------------------------------------------------------------------

void handleHomeTap(
    uint16_t x,
    uint16_t y
) {
    // Status bar -> Settings
    if (y < 28) {
        Serial.println("UI: opening Settings");
        showSettings();
        return;
    }

    // PC
    if (
        x >= 8 &&
        x <= 116 &&
        y >= 36 &&
        y <= 66
    ) {
        selectedDevice = RemoteDevice::PC;

        Serial.println("UI: selected PC");

        showHome();
        return;
    }

    // Roku
    if (
        x >= 124 &&
        x <= 232 &&
        y >= 36 &&
        y <= 66
    ) {
        selectedDevice = RemoteDevice::Roku;

        Serial.println("UI: selected Roku");

        showHome();
        return;
    }

    // Power
    if (
        x >= 8 &&
        x <= 78 &&
        y >= 74 &&
        y <= 106
    ) {
        sendSelectedCommand("power");
    }

    // Home
    else if (
        x >= 85 &&
        x <= 155 &&
        y >= 74 &&
        y <= 106
    ) {
        sendSelectedCommand("home");
    }

    // Back
    else if (
        x >= 162 &&
        x <= 232 &&
        y >= 74 &&
        y <= 106
    ) {
        sendSelectedCommand("back");
    }

    // Up
    else if (
        x >= 88 &&
        x <= 152 &&
        y >= 113 &&
        y <= 147
    ) {
        sendSelectedCommand("up");
    }

    // Left
    else if (
        x >= 17 &&
        x <= 81 &&
        y >= 151 &&
        y <= 189
    ) {
        sendSelectedCommand("left");
    }

    // OK
    else if (
        x >= 88 &&
        x <= 152 &&
        y >= 151 &&
        y <= 189
    ) {
        sendSelectedCommand("ok");
    }

    // Right
    else if (
        x >= 159 &&
        x <= 223 &&
        y >= 151 &&
        y <= 189
    ) {
        sendSelectedCommand("right");
    }

    // Down
    else if (
        x >= 88 &&
        x <= 152 &&
        y >= 193 &&
        y <= 227
    ) {
        sendSelectedCommand("down");
    }

    // Previous
    else if (
        x >= 8 &&
        x <= 78 &&
        y >= 235 &&
        y <= 267
    ) {
        sendSelectedCommand("previous");
    }

    // Play / Pause
    else if (
        x >= 85 &&
        x <= 155 &&
        y >= 235 &&
        y <= 267
    ) {
        sendSelectedCommand("play_pause");
    }

    // Next
    else if (
        x >= 162 &&
        x <= 232 &&
        y >= 235 &&
        y <= 267
    ) {
        sendSelectedCommand("next");
    }

    // Volume -
    else if (
        x >= 8 &&
        x <= 78 &&
        y >= 275 &&
        y <= 311
    ) {
        sendSelectedCommand("volume_down");
    }

    // Mute
    else if (
        x >= 85 &&
        x <= 155 &&
        y >= 275 &&
        y <= 311
    ) {
        sendSelectedCommand("mute");
    }

    // Volume +
    else if (
        x >= 162 &&
        x <= 232 &&
        y >= 275 &&
        y <= 311
    ) {
        sendSelectedCommand("volume_up");
    }
}


// -----------------------------------------------------------------------------
// Settings touch handling
// -----------------------------------------------------------------------------

void handleSettingsTap(
    uint16_t x,
    uint16_t y
) {
    // Back
    if (
        y < 36 &&
        x < 75
    ) {
        Serial.println("UI: returning Home");

        showHome();
        return;
    }

    // Brightness
    if (
        x >= 14 &&
        x <= 226 &&
        y >= 70 &&
        y <= 105
    ) {
        int value = map(
            x,
            14,
            226,
            0,
            100
        );

        uiBrightness = constrain(
            value,
            0,
            100
        );

        setDisplayBrightness(
            uiBrightness
        );

        updateBrightnessSlider(
            uiBrightness
        );

        prefs.putUChar(
            "brightness",
            uiBrightness
        );

        Serial.printf(
            "UI: brightness = %u%%\n",
            uiBrightness
        );

        return;
    }

    // Sleep timer
    if (
        x >= 14 &&
        x <= 226 &&
        y >= 138 &&
        y <= 175
    ) {
        int value = map(
            x,
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

        updateSleepSlider(
            uiSleepSeconds
        );

        prefs.putUShort(
            "sleep_sec",
            uiSleepSeconds
        );

        Serial.printf(
            "UI: sleep timer = %u sec\n",
            uiSleepSeconds
        );

        return;
    }
}


// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    // Latch battery power immediately.
    pinMode(PWR_CONTROL_PIN, OUTPUT);
    digitalWrite(PWR_CONTROL_PIN, HIGH);

    Serial.begin(115200);
    delay(1000);

    Serial.printf(
        "\nUniversal Remote ESP32 Bootstrap %s\n",
        RemoteConfig::FIRMWARE_VERSION
    );

    Serial.println(
        "Target: Waveshare ESP32-S3-Touch-LCD-2.8"
    );

    analogReadResolution(12);

    // Preferences must be available before applying saved UI values.
    prefs.begin("remote", false);

    uiBrightness = prefs.getUChar(
        "brightness",
        75
    );

    uiSleepSeconds = prefs.getUShort(
        "sleep_sec",
        30
    );

    autoUpdate = prefs.getBool(
        "auto_update",
        RemoteConfig::DEFAULT_AUTO_UPDATE
    );

    initDisplay();
    setDisplayBrightness(uiBrightness);

    initTouch();

    OtaClient::begin();

    connectWifi();

    printInfo();

    showHome();

    // Wait one OTA interval before automatic checking.
    lastUpdateCheck = millis();
}


// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    serialConsole();

    RemoteTouchPoint point = readTouch();

    bool newTap =
        point.touched &&
        !wasTouching;

    if (newTap) {
        if (currentScreen == ScreenMode::Home) {
            handleHomeTap(
                point.x,
                point.y
            );
        }
        else if (
            currentScreen == ScreenMode::Settings
        ) {
            handleSettingsTap(
                point.x,
                point.y
            );
        }
    }

    wasTouching = point.touched;

    uint32_t now = millis();

    // Wi-Fi reconnect.
    if (
        WiFi.status() != WL_CONNECTED &&
        now - lastWifiAttempt >=
            RemoteConfig::WIFI_RETRY_INTERVAL_MS
    ) {
        lastWifiAttempt = now;

        connectWifi();
    }

    // OTA.
    if (
        autoUpdate &&
        WiFi.status() == WL_CONNECTED &&
        now - lastUpdateCheck >=
            RemoteConfig::OTA_CHECK_INTERVAL_MS
    ) {
        lastUpdateCheck = now;

        checkUpdate(true);
    }

    // Battery percentage stays present and reasonably current
    // on both Home and Settings without redrawing the screen.
    if (
        now - lastBatteryUpdate >=
            BATTERY_UPDATE_INTERVAL_MS
    ) {
        refreshBatteryStatus();
    }

    delay(5);
}