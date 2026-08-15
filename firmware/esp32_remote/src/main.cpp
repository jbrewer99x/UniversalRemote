#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "config.h"
#include "ota_client.h"
#include "secrets.h"
#include "display.h"
#include "touch.h"
#include "remote_api.h"
#include "imu.h"
#include "sd_storage.h"
#include "sd_updater.h"
#include "audio_player.h"
#include "sound_effects.h"
#define PWR_KEY_PIN 6
#define PWR_CONTROL_PIN  7
#define BAT_ADC_PIN      8

Preferences prefs;

bool autoUpdate = RemoteConfig::DEFAULT_AUTO_UPDATE;
bool wasTouching = false;
bool screenSleeping = false;
bool soundTestActive = false;
bool findRemoteActive = false;
uint32_t lastRemoteCommandPoll = 0;
static constexpr uint32_t REMOTE_COMMAND_POLL_INTERVAL_MS = 5000;
size_t soundTestIndex = 0;

uint32_t lastWifiAttempt = 0;
uint32_t lastUpdateCheck = 0;
uint32_t lastBatteryUpdate = 0;
uint32_t lastActivityAt = 0;
uint32_t sleepStartedAt = 0;
uint32_t lastImuCheck = 0;

uint8_t uiBrightness = 75;
uint16_t uiSleepSeconds = 30;

static constexpr uint32_t BATTERY_UPDATE_INTERVAL_MS = 30000;
static constexpr uint32_t IMU_CHECK_INTERVAL_MS = 100;
static constexpr uint32_t IMU_WAKE_GRACE_MS = 750;
static constexpr float IMU_WAKE_THRESHOLD_G = 0.12f;

bool haveLastImu = false;
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

enum class ScreenMode { Home, Settings };
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

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < RemoteConfig::WIFI_CONNECT_TIMEOUT_MS) {
        delay(300);
        Serial.print(".");
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
    uint32_t totalMv = 0;
    for (int i = 0; i < 8; i++) {
        totalMv += analogReadMilliVolts(BAT_ADC_PIN);
        delay(2);
    }

    float adcVolts = (totalMv / 8.0f) / 1000.0f;
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
    return batteryVoltageToPercent(readBatteryVoltage());
}

void refreshBatteryStatus() {
    updateBatteryStatus(readBatteryPercent());
    lastBatteryUpdate = millis();
    checkBatterySounds(readBatteryPercent());
}


void serviceRemoteCommands() {
    uint32_t now = millis();

    if (
        now - lastRemoteCommandPoll <
        REMOTE_COMMAND_POLL_INTERVAL_MS
    ) {
        return;
    }

    lastRemoteCommandPoll = now;

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    String command;

    if (!checkRemoteCommand(command)) {
        return;
    }

    if (command.length() == 0) {
        return;
    }

    Serial.printf(
        "Remote API: received command: %s\n",
        command.c_str()
    );

    if (command == "find_remote") {
        startFindRemote();
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
        setAudioVolume(DEFAULT_VOLUME);
        return;
    }

    if (!isAudioPlaying()) {
        playWav("/content/findme.wav");
    }
}

void showHome() {
    currentScreen = ScreenMode::Home;
    displayStatus(
        WiFi.status() == WL_CONNECTED,
        WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0",
        "Ready",
        selectedDevice == RemoteDevice::PC
    );
    refreshBatteryStatus();
}

void showSettings() {
    currentScreen = ScreenMode::Settings;
    displaySettings(uiBrightness, uiSleepSeconds);
    refreshBatteryStatus();
}

void noteActivity() {
    lastActivityAt = millis();
}

void primeImuBaseline() {
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

    screenSleeping = true;
    sleepStartedAt = millis();
    primeImuBaseline();

    Serial.printf("Sleep: screen off after %u sec inactivity\n", uiSleepSeconds);
    setDisplayBrightness(0);
}

void wakeScreen(const char* reason) {
    if (!screenSleeping) return;

    screenSleeping = false;
    uint8_t wakeBrightness = uiBrightness == 0 ? 1 : uiBrightness;
    setDisplayBrightness(wakeBrightness);
    noteActivity();
    refreshBatteryStatus();

    Serial.printf("Sleep: woke by %s\n", reason);
    primeImuBaseline();
}

bool imuWakeMotionDetected() {
    uint32_t now = millis();
    if (now - lastImuCheck < IMU_CHECK_INTERVAL_MS) return false;
    lastImuCheck = now;

    float x, y, z;
    if (!readImuAcceleration(x, y, z)) return false;

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
    if (maxDelta >= IMU_WAKE_THRESHOLD_G) {
        Serial.printf("IMU: wake motion delta=%.3f\n", maxDelta);
        return true;
    }
    return false;
}

void printInfo() {
    Serial.printf("Firmware: %s\n", RemoteConfig::FIRMWARE_VERSION);
    Serial.printf("Auto update: %s\n", autoUpdate ? "ON" : "OFF");
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
else if (c == 'p') {
    playWav(
        "/content/oh-bloody-hell.wav"
    );
}
else if (c == 's') {
    SdUpdater::check();
}
else if (c == 'a') {
        autoUpdate = !autoUpdate;
        prefs.putBool("auto_update", autoUpdate);
        Serial.printf("Auto update: %s\n", autoUpdate ? "ON" : "OFF");
    } else if (c == 'r') {
        ESP.restart();
    } else if (c == 'h' || c == '?') {
       Serial.println(
    "h help | i info | c check | u update | s sync SD | "
    "p play WAV | a auto-update toggle | r reboot");
    }
}

void sendSelectedCommand(const char* command) {
    const char* device = selectedDeviceName();
    Serial.printf("UI: %s -> %s\n", device, command);
    sendRemoteCommand(device, command);
}

void handleHomeTap(uint16_t x, uint16_t y) {
    if (y < 28) {
        Serial.println("UI: opening Settings");
        showSettings();
        return;
    }

    if (x >= 8 && x <= 116 && y >= 36 && y <= 66) {
        playSoundEffect(SoundEffect::PcSelected);
        selectedDevice = RemoteDevice::PC;
        Serial.println("UI: selected PC");
        updateDeviceSelector(true);
        return;
    }

    if (x >= 124 && x <= 232 && y >= 36 && y <= 66) {
        playSoundEffect(SoundEffect::RokuSelected);
        selectedDevice = RemoteDevice::Roku;
        Serial.println("UI: selected Roku");
        updateDeviceSelector(false);
        return;
    }

    if (x >= 8 && x <= 78 && y >= 74 && y <= 106) sendSelectedCommand("power");
    else if (x >= 85 && x <= 155 && y >= 74 && y <= 106) sendSelectedCommand("home");
    else if (x >= 162 && x <= 232 && y >= 74 && y <= 106) sendSelectedCommand("back");
    else if (x >= 88 && x <= 152 && y >= 113 && y <= 147) sendSelectedCommand("up");
    else if (x >= 17 && x <= 81 && y >= 151 && y <= 189) sendSelectedCommand("left");
    else if (x >= 88 && x <= 152 && y >= 151 && y <= 189) sendSelectedCommand("ok");
    else if (x >= 159 && x <= 223 && y >= 151 && y <= 189) sendSelectedCommand("right");
    // Previous
else if (
    x >= 17 &&
    x <= 81 &&
    y >= 193 &&
    y <= 227
) {
    sendSelectedCommand("previous");
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

// Next
else if (
    x >= 159 &&
    x <= 223 &&
    y >= 193 &&
    y <= 227
) {
    sendSelectedCommand("next");
}

// Rewind
else if (
    x >= 8 &&
    x <= 78 &&
    y >= 235 &&
    y <= 267
) {
    sendSelectedCommand("rewind");
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

// Fast Forward
else if (
    x >= 162 &&
    x <= 232 &&
    y >= 235 &&
    y <= 267
) {
    sendSelectedCommand("fast_forward");
}
    else if (x >= 8 && x <= 78 && y >= 275 && y <= 311) sendSelectedCommand("volume_down");
    else if (x >= 85 && x <= 155 && y >= 275 && y <= 311) sendSelectedCommand("mute");
    else if (x >= 162 && x <= 232 && y >= 275 && y <= 311) sendSelectedCommand("volume_up");
}

void handleSettingsTap(uint16_t x, uint16_t y) {
    if (y < 36 && x < 75) {
        Serial.println("UI: returning Home");
        showHome();
        return;
    }

    if (x >= 14 && x <= 226 && y >= 70 && y <= 105) {
        int value = map(x, 14, 226, 0, 100);
        uiBrightness = constrain(value, 0, 100);
        setDisplayBrightness(uiBrightness);
        updateBrightnessSlider(uiBrightness);
        prefs.putUChar("brightness", uiBrightness);
        Serial.printf("UI: brightness = %u%%\n", uiBrightness);
        return;
    }

    if (x >= 14 && x <= 226 && y >= 138 && y <= 175) {
        int value = map(x, 14, 226, 2, 120);
        uiSleepSeconds = constrain(value, 2, 120);
        updateSleepSlider(uiSleepSeconds);
        prefs.putUShort("sleep_sec", uiSleepSeconds);
        noteActivity();
        Serial.printf("UI: sleep timer = %u sec\n", uiSleepSeconds);
        return;
    }
    // Play Sounds
    if (
        x >= 20 &&
        x <= 220 &&
        y >= 265 &&
        y <= 305
)   {
    startSoundTest();
    return;
}
}

void setup() {
    pinMode(PWR_CONTROL_PIN, OUTPUT);
    digitalWrite(PWR_CONTROL_PIN, HIGH);

    Serial.begin(115200);
    delay(1000);

    Serial.printf("\nUniversal Remote ESP32 Bootstrap %s\n",
                  RemoteConfig::FIRMWARE_VERSION);
    Serial.println("Target: Waveshare ESP32-S3-Touch-LCD-2.8");

    analogReadResolution(12);

    prefs.begin("remote", false);
    uiBrightness = prefs.getUChar("brightness", 75);
    uiSleepSeconds = prefs.getUShort("sleep_sec", 30);
    autoUpdate = prefs.getBool("auto_update", RemoteConfig::DEFAULT_AUTO_UPDATE);

    initDisplay();
    setDisplayBrightness(uiBrightness);
    initTouch();
    initImu();

    OtaClient::begin();
    connectWifi();
    printInfo();
    showHome();

    lastUpdateCheck = millis();
    lastActivityAt = millis();
    primeImuBaseline();

    if (initSdCard()) {
        testSdCard();
}
initAudio();
}

void loop() {
    serialConsole();
    serviceAudio();
    serviceRemoteCommands();

    RemoteTouchPoint point = readTouch();
    uint32_t now = millis();

    bool imuMotion = false;

    if (
        findRemoteActive ||
        (
            screenSleeping &&
            now - sleepStartedAt >= IMU_WAKE_GRACE_MS
        )
    ) {
        imuMotion = imuWakeMotionDetected();
    }

    serviceFindRemote(imuMotion);
    serviceSoundTest();
    serviceLowBatteryShutdown();
    if (screenSleeping) {
    if (point.touched) {
        wakeScreen("touch");
        wasTouching = true;
    } else if (imuMotion) {
        wakeScreen("motion");
    }

        if (!point.touched) {
            wasTouching = false;
        }
    } else {
        if (point.touched) {
            noteActivity();
        }

        bool newTap = point.touched && !wasTouching;

        if (newTap) {
            if (currentScreen == ScreenMode::Home) {
                handleHomeTap(point.x, point.y);
            } else if (currentScreen == ScreenMode::Settings) {
                handleSettingsTap(point.x, point.y);
            }
        }

        wasTouching = point.touched;

        if (
            uiSleepSeconds > 0 &&
            now - lastActivityAt >= ((uint32_t)uiSleepSeconds * 1000UL)
        ) {
            enterScreenSleep();
        }
    }

    if (
        WiFi.status() != WL_CONNECTED &&
        now - lastWifiAttempt >= RemoteConfig::WIFI_RETRY_INTERVAL_MS
    ) {
        lastWifiAttempt = now;
        connectWifi();
    }

if (
    autoUpdate &&
    WiFi.status() == WL_CONNECTED &&
    now - lastUpdateCheck >= RemoteConfig::OTA_CHECK_INTERVAL_MS
) {
    lastUpdateCheck = now;

    checkUpdate(true);
    SdUpdater::check();
}

    if (
        !screenSleeping &&
        now - lastBatteryUpdate >= BATTERY_UPDATE_INTERVAL_MS
    ) {
        refreshBatteryStatus();
    }

    delay(5);
}
void serviceLowBatteryShutdown() {
    if (!lowBatteryShutdownPending) {
        return;
    }

    // Let serviceAudio() finish playing the shutdown message.
    if (isAudioPlaying()) {
        return;
    }

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