#pragma once
#include <stdint.h>
namespace RemoteConfig {
// Preserve the board's existing divider and measured calibration correction.
inline constexpr float BATTERY_DIVIDER_RATIO = 3.0f;
inline constexpr float BATTERY_CALIBRATION = 1.0f / 0.990476f;
inline constexpr char FIRMWARE_VERSION[] = "0.2.26";
inline constexpr char OTA_MANIFEST_PATH[] = "/api/firmware/manifest";
inline constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
inline constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 25000;
inline constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
// Find Remote can take up to this interval to arrive while the screen is off.
// Touch/motion wake restores the normal five-second polling cadence immediately.
inline constexpr uint32_t SCREEN_OFF_POLL_INTERVAL_MS = 20000;
inline constexpr uint32_t SCREEN_OFF_WIFI_RETRY_INTERVAL_MS = 60000;
}

