#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <driver/gpio.h>

#include "config.h"
#include "battery.h"
#include "ota_client.h"
#include "secrets.h"
#include "display.h"
#include "touch.h"
#include "ui_policy.h"
#include "remote_api.h"
#include "imu.h"
#include "sd_storage.h"
#include "sd_updater.h"
#include "audio_player.h"
#include "sound_effects.h"
#include "power_manager.h"
#define PWR_KEY_PIN 6
#define PWR_CONTROL_PIN  7
#define BAT_ADC_PIN      8
#define WAKE_TOUCH_PIN   4
#define WAKE_BUTTON_PIN 18

Preferences prefs;

WakeTouchGate wakeTouchGate;
SavedSetting<uint8_t> savedBrightness;
SavedSetting<uint16_t> savedSleep;
enum class Maintenance { None, CheckFirmware, InstallFirmware, Sd, All };
Maintenance maintenance = Maintenance::None;
uint32_t maintenanceRequestedAt = 0;
void saveSettings();
void requestMaintenance(Maintenance job);
void serviceMaintenance();
void handleDisplayActions();
bool screenSleeping = false;
bool lightSleepPending = false;
uint32_t lightSleepRequestedAt = 0;
bool wakeButtonArmed = false;
bool wakeButtonRawDown = false;
uint32_t wakeButtonChangedAt = 0;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
static constexpr uint32_t SLEEP_PREPARE_TIMEOUT_MS = 10000;
bool soundTestActive = false;
bool findRemoteActive = false;
size_t soundTestIndex = 0;

uint32_t lastWifiAttempt = 0;
uint32_t lastBatteryUpdate = 0;
uint32_t lastActivityAt = 0;
uint32_t sleepStartedAt = 0;
uint32_t lastImuCheck = 0;

uint8_t uiBrightness = 75;
uint16_t uiSleepSeconds = 30;

static constexpr uint32_t BATTERY_UPDATE_INTERVAL_MS = 30000;
// 20 Hz polling catches shaking while awake or screen-off.
static constexpr uint32_t IMU_CHECK_INTERVAL_MS = 50;
static constexpr uint32_t IMU_WAKE_GRACE_MS = 750;
static constexpr float IMU_WAKE_THRESHOLD_G = 0.12f;
// Preserve screen-off + IMU wake for one hour before true light sleep.
static constexpr uint32_t LIGHT_SLEEP_AFTER_MS = 60UL * 60UL * 1000UL;

bool haveLastImu = false;
bool shakeActive = false;
bool shakePlayed = false;
uint32_t shakeStartedAt = 0;
uint32_t lastShakeAt = 0;
static constexpr float SHAKE_THRESHOLD_G = 0.6f;
static constexpr uint32_t SHAKE_GAP_MS = 400;
static constexpr uint32_t SHAKE_REARM_MS = 1000;
float lastImuX = 0.0f;
float lastImuY = 0.0f;
float lastImuZ = 0.0f;
bool battery30WarningPlayed = false;
bool battery20WarningPlayed = false;
bool battery10WarningPlayed = false;
bool lowBatteryShutdownPending = false;
void serviceLowBatteryShutdown();
void serviceSoundTest();
void startSoundTest();
void primeImuBaseline();
bool imuWakeMotionDetected();
void startFindRemote();

const char* SOUND_TEST_FILES[] = {
    "/content/hell-yeah-brother.wav",
    "/content/i-have-some-new-tricks.wav",
    "/content/im-getting-really-sleepy.wav",
    "/content/im-shutting-down.wav",
    "/content/my-battery-is-dangerously-low.wav",
    "/content/oh-bloody-hell-can-you-please-stop.wav",
    "/content/oh-bloody-hell.wav",
    "/content/pc.wav",
    "/content/roku.wav",
    "/content/starting-up.wav",
    "/content/supercalafragalisticexbealladocious.wav"
};

constexpr size_t SOUND_TEST_COUNT =
    sizeof(SOUND_TEST_FILES) / sizeof(SOUND_TEST_FILES[0]);

enum class ScreenMode { Home, Settings, Lights };
ScreenMode currentScreen = ScreenMode::Home;

enum class RemoteDevice { PC, Roku };
RemoteDevice selectedDevice = RemoteDevice::PC;

const char* selectedDeviceName() {
    return selectedDevice == RemoteDevice::PC ? "pc" : "roku";
}

bool connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // Associated modem sleep retains inbound reachability; do not use MAX_MODEM
    // or reduce RF transmit power, which could affect latency/range.
    if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK)
        Serial.println("Power: could not enable Wi-Fi modem sleep");

    // WiFi.begin starts association in the background. Never hold up touch,
    // audio, or screen restoration while waiting for an access point.
    lastWifiAttempt = millis();
    Serial.println(" (background connection)");
    return WiFi.status() == WL_CONNECTED;
}
void checkBatterySounds(uint8_t percent) {
    if (percent >= 40) {
        battery30WarningPlayed = false;
        battery20WarningPlayed = false;
        battery10WarningPlayed = false;
        return;
    }

    if (percent <= 10 && !battery10WarningPlayed) {
        battery10WarningPlayed = true;
        battery20WarningPlayed = true;
        battery30WarningPlayed = true;

        playSoundEffect(SoundEffect::Battery10);
        lowBatteryShutdownPending = true;
        return;
    }

    if (percent <= 20 && !battery20WarningPlayed) {
        battery20WarningPlayed = true;
        battery30WarningPlayed = true;

        playSoundEffect(SoundEffect::Battery20);
        return;
    }

    if (percent <= 30 && !battery30WarningPlayed) {
        battery30WarningPlayed = true;

        playSoundEffect(SoundEffect::Battery30);
    }
}


float readBatteryVoltage() {
    // Reject transient ADC spikes without retaining stale readings after wake.
    uint32_t samples[16];
    for (int i = 0; i < 16; ++i) {
        samples[i] = analogReadMilliVolts(BAT_ADC_PIN);
        for (int j = i; j > 0 && samples[j] < samples[j - 1]; --j) {
            const uint32_t value = samples[j];
            samples[j] = samples[j - 1];
            samples[j - 1] = value;
        }
        serviceAudio();
        delay(2);
    }
    uint32_t totalMv = 0;
    for (int i = 4; i < 12; ++i) totalMv += samples[i];
    return (totalMv / 8000.0f) * RemoteConfig::BATTERY_DIVIDER_RATIO *
           RemoteConfig::BATTERY_CALIBRATION;
}

void refreshBatteryStatus() {
    const float volts = readBatteryVoltage();
    const uint8_t percent = batteryVoltageToPercent(volts);
    updateBatteryStatus(percent, volts);
    lastBatteryUpdate = millis();
    checkBatterySounds(percent);
}


void serviceRemoteCommands() {
    RemoteResult result;
    while (takeRemoteResult(result)) {
        if (!result.success) {
            // Poll backoff is silent; explicit user commands get visible feedback.
            if (!result.poll) {
                displayFeedback(result.expired ? "Command expired. Try again." : "Command failed. Check connection.");
                reportErrorSound();
            }
        } else if (result.poll && !strcmp(result.command, "find_remote") &&
                   !lowBatteryShutdownPending && !lightSleepPending) {
            startFindRemote();
        }
    }
}

void startFindRemote() {
    if (findRemoteActive) {
        return;
    }

    Serial.println("Find Remote: activated");

    findRemoteActive = true;
    primeImuBaseline();

    setAudioVolume(MAX_VOLUME);
    playWav("/content/findme.wav");
}

void serviceFindRemote(bool motionDetected) {
    if (!findRemoteActive) {
        return;
    }

    if (motionDetected) {
        Serial.println("Find Remote: pickup detected");

        findRemoteActive = false;
        stopAudio();
        setAudioVolume(DEFAULT_VOLUME);
        return;
    }

    if (!isAudioPlaying()) {
        playWav("/content/findme.wav");
    }
}

bool queueGoveeLightCommand(const char* command, int v1 = -1, int v2 = -1, int v3 = -1) {
    if (WiFi.status() != WL_CONNECTED) {
        displayFeedback("Wi-Fi offline. Try again.");
        reportErrorSound();
        return false;
    }

    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(2000);
    http.setReuse(false);
    if (!http.begin(String(REMOTE_SERVER_URL) + "/api/command")) {
        displayFeedback("Govee request failed.");
        reportErrorSound();
        return false;
    }

    JsonDocument doc;
    doc["device"] = "living_room_lights";
    doc["command"] = command;

    if (strcmp(command, "color") == 0 && v1 >= 0 && v2 >= 0 && v3 >= 0) {
        JsonObject value = doc["value"].to<JsonObject>();
        value["r"] = v1;
        value["g"] = v2;
        value["b"] = v3;
    } else if (v1 >= 0) {
        doc["value"] = v1;
    }

    String body;
    serializeJson(doc, body);
    http.addHeader("Content-Type", "application/json");
    const int status = http.POST(body);
    const bool ok = status >= 200 && status < 300;
    http.end();

    if (!ok) {
        displayFeedback("Govee command failed.");
        reportErrorSound();
        return false;
    }

    const String feedback = String("Govee: ") + command;
    displayFeedback(feedback.c_str());
    return true;
}

void showHome() {
    saveSettings();
    wakeTouchGate.suppress();
    currentScreen = ScreenMode::Home;
    displayStatus(
        WiFi.status() == WL_CONNECTED,
        WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0",
        "Ready",
        selectedDevice == RemoteDevice::PC
    );
    refreshBatteryStatus();
}

void showLights() {
    wakeTouchGate.suppress();
    currentScreen = ScreenMode::Lights;
    displayLights();
    refreshBatteryStatus();
}

void showSettings() {
    wakeTouchGate.suppress();
    currentScreen = ScreenMode::Settings;
    displaySettings(uiBrightness, uiSleepSeconds);
    refreshBatteryStatus();
}

void noteActivity() {
    lastActivityAt = millis();
}

void primeImuBaseline() {
    shakeActive = false;
    shakePlayed = false;
    float x, y, z;
    if (readImuAcceleration(x, y, z)) {
        lastImuX = x;
        lastImuY = y;
        lastImuZ = z;
        haveLastImu = true;
    } else {
        haveLastImu = false;
    }
}

void enterScreenSleep() {
    if (screenSleeping) return;

    saveSettings();
    suppressDisplayTouch();
    wakeTouchGate.suppress();
    screenSleeping = true;
    Power::setScreenOff(true);
    setRemoteScreenOff(true);
    sleepStartedAt = millis();
    primeImuBaseline();

    Serial.printf("Sleep: screen off after %u sec inactivity\n", uiSleepSeconds);
    setDisplayBrightness(0);
    setDisplaySleeping(true);
}

void wakeScreen(const char* reason) {
    if (!screenSleeping) return;

    screenSleeping = false;
    Power::setScreenOff(false);
    Power::uiActivity();
    setRemoteScreenOff(false);
    setDisplaySleeping(false);
    uint8_t wakeBrightness = uiBrightness == 0 ? 1 : uiBrightness;
    setDisplayBrightness(wakeBrightness);
    noteActivity();
    refreshBatteryStatus();

    Serial.printf("Sleep: woke by %s\n", reason);
    primeImuBaseline();
}


// Require a stable release before accepting another press, including at boot
// and after wake. All timing uses unsigned subtraction for millis() rollover.
bool pollWakeButton() {
    const bool down = digitalRead(WAKE_BUTTON_PIN) == LOW;
    const uint32_t now = millis();
    if (down != wakeButtonRawDown) {
        wakeButtonRawDown = down;
        wakeButtonChangedAt = now;
    }
    if (now - wakeButtonChangedAt < BUTTON_DEBOUNCE_MS) return false;
    if (!down) {
        wakeButtonArmed = true;
        return false;
    }
    if (!wakeButtonArmed) return false;
    wakeButtonArmed = false;
    return true;
}

void enterLightSleep() {
    if (lightSleepPending || findRemoteActive || soundTestActive ||
        isAudioPlaying() || lowBatteryShutdownPending) return;

    enterScreenSleep();
    pauseRemoteNetwork();
    lightSleepPending = true;
    lightSleepRequestedAt = millis();
    Serial.println("Sleep: preparing; waiting for audio and button release");
    playSoundEffect(SoundEffect::Sleeping);
}

void serviceLightSleep() {
    if (!lightSleepPending) return;

    const uint32_t now = millis();
    const bool released = !wakeButtonRawDown &&
        now - wakeButtonChangedAt >= BUTTON_DEBOUNCE_MS;
    if (now - lightSleepRequestedAt >= SLEEP_PREPARE_TIMEOUT_MS) {
        // A stuck button or decoder must not strand the user at a dark screen.
        stopAudio();
        lightSleepPending = false;
        reportErrorSound();
        resumeRemoteNetwork();
        wakeScreen("sleep preparation timed out");
        return;
    }
    if (isAudioPlaying() || !released || !remoteNetworkIdle() || !isDisplaySleeping()) return;

    const gpio_num_t pin = (gpio_num_t)WAKE_BUTTON_PIN;
    esp_err_t result = gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
    if (result == ESP_OK) result = esp_sleep_enable_gpio_wakeup();
    if (result != ESP_OK) {
        gpio_wakeup_disable(pin);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        lightSleepPending = false;
        Serial.printf("Sleep: wake configuration failed: %d\n", (int)result);
        reportErrorSound();
        resumeRemoteNetwork();
        wakeScreen("wake configuration failed");
        return;
    }

    setTouchSleeping(true);
    setImuSleeping(true);
    Power::beforeLightSleep();
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    Serial.printf("Sleep: entering light sleep; GPIO%d=%d\n",
                  WAKE_BUTTON_PIN, digitalRead(WAKE_BUTTON_PIN));
    result = esp_light_sleep_start();
    Power::afterLightSleep();
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    gpio_wakeup_disable(pin);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    lightSleepPending = false;

    // Show the UI immediately, even if the button stays held or Wi-Fi is down.
    wakeButtonArmed = false;
    wakeButtonRawDown = digitalRead(WAKE_BUTTON_PIN) == LOW;
    wakeButtonChangedAt = millis();
    wakeTouchGate.suppress();
    suppressDisplayTouch();
    wakeScreen(result == ESP_OK ? "power button" : "sleep failed");
    setTouchSleeping(false);
    if (!setImuSleeping(false)) initImu();
    primeImuBaseline();
    Serial.printf("Sleep: returned result=%d cause=%d GPIO%d=%d\n",
                  (int)result, (int)cause, WAKE_BUTTON_PIN,
                  digitalRead(WAKE_BUTTON_PIN));
    if (result == ESP_OK && cause == ESP_SLEEP_WAKEUP_GPIO &&
        !lowBatteryShutdownPending) {
        playSoundEffect(SoundEffect::Waking);
    } else if (result != ESP_OK) {
        reportErrorSound();
    }
    connectWifi();
    resumeRemoteNetwork();
}

bool imuWakeMotionDetected() {
    uint32_t now = millis();
    if (now - lastImuCheck < IMU_CHECK_INTERVAL_MS) return false;
    if (now - lastImuCheck > SHAKE_GAP_MS) {
        // Downloads/sleep are unobserved time, not sustained shaking.
        shakeActive = false;
        haveLastImu = false;
    }
    lastImuCheck = now;

    float x, y, z;
    if (!readImuAcceleration(x, y, z)) {
        shakeActive = false;
        haveLastImu = false;
        return false;
    }

    if (!haveLastImu) {
        lastImuX = x;
        lastImuY = y;
        lastImuZ = z;
        haveLastImu = true;
        return false;
    }

    float dx = fabsf(x - lastImuX);
    float dy = fabsf(y - lastImuY);
    float dz = fabsf(z - lastImuZ);

    lastImuX = x;
    lastImuY = y;
    lastImuZ = z;

    float maxDelta = max(dx, max(dy, dz));
    if (now - lastShakeAt >= SHAKE_REARM_MS) shakePlayed = false;
    if (maxDelta >= SHAKE_THRESHOLD_G) {
        if (!shakeActive || now - lastShakeAt > SHAKE_GAP_MS) {
            shakeActive = true;
            shakeStartedAt = now;
        }
        lastShakeAt = now;
        if (!shakePlayed && now - shakeStartedAt > 3000 &&
            !isAudioPlaying() && !findRemoteActive && !soundTestActive &&
            !lowBatteryShutdownPending && !lightSleepPending) {
            shakePlayed = true;
            playSoundEffect(SoundEffect::PleaseStop);
        }
    } else if (now - lastShakeAt > SHAKE_GAP_MS) {
        shakeActive = false;
    }
    if (maxDelta >= IMU_WAKE_THRESHOLD_G) {
        Serial.printf("IMU: wake motion delta=%.3f\n", maxDelta);
        return true;
    }
    return false;
}

void printInfo() {
    Power::printStats();
    Serial.printf("Power: screen-off poll interval=%lu ms\n",
                  (unsigned long)RemoteConfig::SCREEN_OFF_POLL_INTERVAL_MS);
    printDisplayStats();
    Serial.printf("Firmware: %s\n", RemoteConfig::FIRMWARE_VERSION);
    Serial.println("Updates: manual (Settings > Check for updates)");
    Serial.printf("Wi-Fi: %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
    Serial.printf("PSRAM: %u total / %u free\n",
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("Heap free: %u\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("TrueNAS: %s\n", REMOTE_SERVER_URL);

    float batteryVoltage = readBatteryVoltage();
    Serial.printf("Battery: %.2f V / %u%%\n",
                  batteryVoltage,
                  batteryVoltageToPercent(batteryVoltage));
    Serial.printf("Selected device: %s\n", selectedDeviceName());
    Serial.printf("Screen sleep: %s (%u sec)\n",
                  screenSleeping ? "sleeping" : "awake",
                  uiSleepSeconds);
}

void checkUpdate(bool install) {
    requestMaintenance(install ? Maintenance::InstallFirmware : Maintenance::CheckFirmware);
}

void serialConsole() {
    if (!Serial.available()) return;
    char c = Serial.read();

    if (c == 'i') printInfo();
else if (c == 'c') checkUpdate(false);
else if (c == 'u') checkUpdate(true);
else if (c == 'p') {
    playWav(
        "/content/oh-bloody-hell.wav"
    );
}
else if (c == 's') {
    requestMaintenance(Maintenance::Sd);
}
else if (c == 'r') {
        ESP.restart();
    } else if (c == 'h' || c == '?') {
       Serial.println(
    "h help | i info | c check | u update | s sync SD | "
    "p play WAV | r reboot");
    }
}

void sendSelectedCommand(const char* command) {
    const auto result = queueRemoteCommand(selectedDeviceName(), command);
    if (result != RemoteNetwork::Submit::Accepted) {
        displayFeedback(result == RemoteNetwork::Submit::Offline ? "Wi-Fi offline. Try again." :
                        result == RemoteNetwork::Submit::Full ? "Command queue full. Try again." :
                        "Remote busy. Try again.");
        reportErrorSound();
    }
}

void checkSettingsUpdates() {
    if (WiFi.status() != WL_CONNECTED) {
        reportErrorSound();
        displayUpdateStatus("Wi-Fi offline. Try again.");
        return;
    }
    if (lowBatteryShutdownPending) {
        displayUpdateStatus("Battery low. Charge first.");
        return;
    }

    // One audio player/SD card: finish other modes before updating files.
    soundTestActive = false;
    findRemoteActive = false;
    stopAudio();
    setAudioVolume(DEFAULT_VOLUME);

    // Firmware installation reboots immediately, so sync SD content first.
    displayUpdateStatus("Updating SD content...");
    const bool sdOk = SdUpdater::check();
    stopAudio();
    displayUpdateStatus(sdOk ? "Checking firmware..." : "SD failed; checking firmware...");
    const auto result = OtaClient::check(true);
    Serial.printf("Manual update: SD=%s; OTA: %s\n",
                  sdOk ? "ready" : "failed", result.message.c_str());

    if (result.result == OtaClient::Result::UpToDate) {
        displayUpdateStatus(sdOk ? "Updates complete. All current." : "Firmware current. SD failed.");
    } else {
        displayUpdateStatus(sdOk ? "SD ready. Firmware check failed." : "Update failed. Try again.");
    }
    // A long download must not make the screen sleep as soon as it finishes.
    noteActivity();
}

void saveSettings() {
    if (savedBrightness.needsSave(uiBrightness)) {
        const bool ok = prefs.putUChar("brightness", uiBrightness) != 0;
        savedBrightness.committed(uiBrightness, ok);
        if (!ok) { reportErrorSound(); displayFeedback("Could not save brightness."); }
    }
    if (savedSleep.needsSave(uiSleepSeconds)) {
        const bool ok = prefs.putUShort("sleep_sec", uiSleepSeconds) != 0;
        savedSleep.committed(uiSleepSeconds, ok);
        if (!ok) { reportErrorSound(); displayFeedback("Could not save sleep timer."); }
    }
}

void requestMaintenance(Maintenance job) {
    if (maintenance != Maintenance::None || lightSleepPending || lowBatteryShutdownPending) return;
    if (WiFi.status() != WL_CONNECTED) {
        displayFeedback("Wi-Fi offline. Try again.");
        reportErrorSound();
        return;
    }
    wakeScreen("update");
    if (lowBatteryShutdownPending) return;
    saveSettings();
    maintenance = job;
    maintenanceRequestedAt = millis();
    pauseRemoteNetwork();
    suppressDisplayTouch();
    wakeTouchGate.suppress();
    showSettings();
    displayUpdateStatus("Preparing update...");
}

void serviceMaintenance() {
    if (maintenance == Maintenance::None) return;
    // Wait over loop iterations while audio, power checks, and rendering are serviced.
    if (lowBatteryShutdownPending || millis() - maintenanceRequestedAt >= 5000) {
        maintenance = Maintenance::None;
        resumeRemoteNetwork();
        displayUpdateStatus("Update postponed. Try again.");
        noteActivity();
        return;
    }
    if (!remoteNetworkIdle()) return;
    Power::HeavyWork performance;
    const auto job = maintenance;
    soundTestActive = findRemoteActive = false;
    stopAudio();
    setAudioVolume(DEFAULT_VOLUME);
    if (job == Maintenance::All) checkSettingsUpdates();
    else if (job == Maintenance::Sd) {
        displayUpdateStatus("Updating SD content...");
        const bool ok = SdUpdater::check();
        displayUpdateStatus(ok ? "SD content ready." : "SD update failed. Try again.");
    } else {
        displayUpdateStatus("Checking firmware...");
        const auto result = OtaClient::check(job == Maintenance::InstallFirmware);
        Serial.printf("OTA: %s\n", result.message.c_str());
        displayUpdateStatus(result.message.c_str());
    }
    maintenance = Maintenance::None;
    resumeRemoteNetwork();
    suppressDisplayTouch();
    wakeTouchGate.suppress();
    noteActivity();
}

bool handleScreenSwipeGesture() {
    static bool tracking = false;
    static int startX = 0;
    static int startY = 0;
    static int lastX = 0;
    static int lastY = 0;

    RemoteTouchPoint point = readTouch();
    if (point.touched) {
        if (!tracking) {
            tracking = true;
            startX = point.x;
            startY = point.y;
            lastX = point.x;
            lastY = point.y;
        } else {
            lastX = point.x;
            lastY = point.y;
        }
        return false;
    }

    if (!tracking) {
        return false;
    }

    const int dx = lastX - startX;
    const int dy = lastY - startY;
    const bool horizontal = abs(dx) > 70 && abs(dy) < 50;
    tracking = false;
    if (!horizontal) {
        return false;
    }

    if (currentScreen == ScreenMode::Home && dx > 0 && startX < 90) {
        showLights();
        return true;
    }
    if (currentScreen == ScreenMode::Lights && dx < 0 && startX > 150) {
        showHome();
        return true;
    }
    return false;
}

void handleDisplayActions() {
    DisplayAction action;
    while (takeDisplayAction(action)) {
        const char* name = action.name;
        noteActivity();
        if (!strcmp(name, "settings")) showSettings();
        else if (!strcmp(name, "show_home")) showHome();
        else if (!strcmp(name, "show_lights")) showLights();
        else if (!strcmp(name, "pc") || !strcmp(name, "roku")) {
            const bool choosePc = !strcmp(name, "pc");
            selectedDevice = choosePc ? RemoteDevice::PC : RemoteDevice::Roku;
            updateDeviceSelector(choosePc);
            playSoundEffect(choosePc ? SoundEffect::PcSelected : SoundEffect::RokuSelected);
        } else if (!strcmp(name, "brightness")) {
            uiBrightness = constrain(action.value, 0, 100);
            setDisplayBrightness(uiBrightness);
            updateBrightnessSlider(uiBrightness);
        } else if (!strcmp(name, "sleep")) {
            uiSleepSeconds = constrain(action.value, 2, 120);
            updateSleepSlider(uiSleepSeconds);
        } else if (!strcmp(name, "save_settings")) saveSettings();
        else if (!strcmp(name, "updates")) requestMaintenance(Maintenance::All);
        else if (!strcmp(name, "sounds")) startSoundTest();
        else if (!strcmp(name, "govee_on")) queueGoveeLightCommand("power", 1);
        else if (!strcmp(name, "govee_off")) queueGoveeLightCommand("power", 0);
        else if (!strcmp(name, "govee_brightness")) queueGoveeLightCommand("brightness", action.value);
        else if (!strcmp(name, "govee_temperature")) queueGoveeLightCommand("temperature", action.value);
        else if (!strcmp(name, "govee_warm")) queueGoveeLightCommand("temperature", 3000);
        else if (!strcmp(name, "govee_cool")) queueGoveeLightCommand("temperature", 6500);
        else if (!strcmp(name, "govee_red")) queueGoveeLightCommand("color", 255, 0, 0);
        else if (!strcmp(name, "govee_green")) queueGoveeLightCommand("color", 0, 255, 0);
        else if (!strcmp(name, "govee_blue")) queueGoveeLightCommand("color", 0, 0, 255);
        else if (!strcmp(name, "govee_party")) queueGoveeLightCommand("crazy_toggle");
        else if (!strcmp(name, "govee_crazy_toggle")) queueGoveeLightCommand("crazy_toggle");
        else sendSelectedCommand(name);
    }
}

void setup() {
    pinMode(PWR_CONTROL_PIN, OUTPUT);
    digitalWrite(PWR_CONTROL_PIN, HIGH);
    pinMode(WAKE_BUTTON_PIN, INPUT_PULLUP);
    Serial.begin(115200);
    Power::begin();
    delay(1000);

    Serial.printf("\nUniversal Remote ESP32 Bootstrap %s\n",
                  RemoteConfig::FIRMWARE_VERSION);
    Serial.println("Target: Waveshare ESP32-S3-Touch-LCD-2.8");

    analogReadResolution(12);

    if (!prefs.begin("remote", false)) reportErrorSound();
    uiBrightness = prefs.getUChar("brightness", 75);
    uiSleepSeconds = prefs.getUShort("sleep_sec", 30);

    savedBrightness.saved = uiBrightness;
    savedSleep.saved = uiSleepSeconds;
    if (!initDisplay()) reportErrorSound();
    setDisplayBrightness(uiBrightness);
    initTouch();
    initImu();

    OtaClient::begin();
    connectWifi();
    printInfo();
    lastActivityAt = millis();
    primeImuBaseline();

    if (initSdCard()) {
        testSdCard();
    }
    initAudio();
    playSoundEffect(SoundEffect::Startup);
    finishAudioPlayback();
    if (!initRemoteNetwork()) reportErrorSound();
    showHome(); // Battery warnings now run after audio and SD are ready.
    noteActivity();
}

void loop() {
    Power::service();
    serviceDisplayPower();
    serviceAudio();
    if (lowBatteryShutdownPending) {
        serviceLowBatteryShutdown();
        delay(1);
        return;
    }
    serialConsole();
    const bool buttonPressed = pollWakeButton();
    if (buttonPressed && !lightSleepPending && maintenance == Maintenance::None) enterLightSleep();
    if (lightSleepPending) {
        serviceLightSleep();
        delay(5);
        return; // Resample touch and time after sleep; never use pre-sleep input.
    }

    if (maintenance != Maintenance::None) {
        readTouch(); // Drain reports so touches during maintenance cannot become commands.
        serviceDisplay();
        serviceMaintenance();
        delay(5);
        return;
    }
    serviceRemoteCommands();
    static bool displayedWifiConnected = false;
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (!screenSleeping && currentScreen == ScreenMode::Home &&
        wifiConnected != displayedWifiConnected) {
        updateWifiStatus(wifiConnected);
        displayedWifiConnected = wifiConnected;
    }

    RemoteTouchPoint point = readTouch();
    if (!screenSleeping) {
        const bool triggered = handleScreenSwipeGesture();
        if (triggered) {
            point = {false, 0, 0};
        }
    }
    uint32_t now = millis();
    const bool detectedMotion = imuWakeMotionDetected();
    const bool imuMotion = detectedMotion && (findRemoteActive ||
        (screenSleeping && now - sleepStartedAt >= IMU_WAKE_GRACE_MS));

    serviceFindRemote(imuMotion);
    serviceSoundTest();
    serviceLowBatteryShutdown();
    if (screenSleeping) {
        if (point.touched) {
            wakeScreen("touch");
            wakeTouchGate.suppress();
        } else if (imuMotion) wakeScreen("motion");
        wakeTouchGate.accept(point.touched);
        if (screenSleeping && now - lastActivityAt >= LIGHT_SLEEP_AFTER_MS) {
            enterLightSleep();
            now = millis();
        }
    } else {
        if (point.touched) noteActivity();
        setDisplayTouch(wakeTouchGate.accept(point.touched), point.x, point.y);
        serviceDisplay();
        handleDisplayActions();
        if (uiSleepSeconds > 0 && maintenance == Maintenance::None &&
            millis() - lastActivityAt >= uint32_t(uiSleepSeconds) * 1000UL) enterScreenSleep();
    }

    if (
        WiFi.status() != WL_CONNECTED &&
        now - lastWifiAttempt >= (screenSleeping ? RemoteConfig::SCREEN_OFF_WIFI_RETRY_INTERVAL_MS :
                                                  RemoteConfig::WIFI_RETRY_INTERVAL_MS)
    ) {
        lastWifiAttempt = now;
        connectWifi();
    }


    if (
        !screenSleeping &&
        now - lastBatteryUpdate >= BATTERY_UPDATE_INTERVAL_MS
    ) {
        refreshBatteryStatus();
    }

    if (!lowBatteryShutdownPending && !lightSleepPending &&
        !findRemoteActive && !soundTestActive) serviceErrorSound();

    setRemotePolling(!lightSleepPending && !lowBatteryShutdownPending &&
                     maintenance == Maintenance::None && !isAudioPlaying());

    // Slower loop while the display is dark.
    delay(screenSleeping ? 20 : 5);
}
void serviceLowBatteryShutdown() {
    if (!lowBatteryShutdownPending) {
        return;
    }

    pauseRemoteNetwork();
    saveSettings();
    // Let serviceAudio() finish playing the shutdown message.
    if (isAudioPlaying()) {
        return;
    }

    finishAudioPlayback();
    Serial.println("Battery critical: shutting down");

    lowBatteryShutdownPending = false;

    // Turn off display first.
    setDisplayBrightness(0);

    delay(100);

    // Release the board's power latch.
    digitalWrite(PWR_CONTROL_PIN, LOW);

    // We should never get here if power actually shuts off.
    while (true) {
        delay(1000);
    }
}
void startSoundTest() {
    if (soundTestActive) {
        return;
    }

    Serial.println("Audio: starting sound test");

    soundTestActive = true;
    soundTestIndex = 0;

    playWav(
        SOUND_TEST_FILES[soundTestIndex]
    );
}

void serviceSoundTest() {
    if (!soundTestActive) {
        return;
    }

    if (isAudioPlaying()) {
        return;
    }

    soundTestIndex++;

    if (soundTestIndex >= SOUND_TEST_COUNT) {
        soundTestActive = false;

        Serial.println(
            "Audio: sound test complete"
        );

        return;
    }

    Serial.printf(
        "Audio: playing %s\n",
        SOUND_TEST_FILES[soundTestIndex]
    );

    playWav(
        SOUND_TEST_FILES[soundTestIndex]
    );
}
