#pragma once
#include <Arduino.h>
namespace RemoteConfig {
inline constexpr char FIRMWARE_VERSION[] = "0.1.0";
inline constexpr char OTA_MANIFEST_PATH[] = "/api/firmware/manifest";
inline constexpr uint32_t OTA_CHECK_INTERVAL_MS = 60000;
inline constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
inline constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 25000;
inline constexpr uint32_t HTTP_CONNECT_TIMEOUT_MS = 5000;
inline constexpr uint32_t HTTP_REQUEST_TIMEOUT_MS = 30000;
inline constexpr bool DEFAULT_AUTO_UPDATE = true;
}
