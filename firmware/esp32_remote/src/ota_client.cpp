#include "ota_client.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "config.h"
#include "secrets.h"

namespace {
String joinUrl(String base, const String& path) {
    if (path.startsWith("http://") || path.startsWith("https://")) return path;
    if (base.endsWith("/")) base.remove(base.length() - 1);
    if (!path.startsWith("/")) base += "/";
    base += path;
    return base;
}

int versionPart(const String& s, int part) {
    int start = 0;
    for (int i = 0; i < part; ++i) {
        int dot = s.indexOf('.', start);
        if (dot < 0) return 0;
        start = dot + 1;
    }
    int end = s.indexOf('.', start);
    if (end < 0) end = s.length();
    String x = s.substring(start, end);
    int dash = x.indexOf('-');
    if (dash >= 0) x = x.substring(0, dash);
    return x.toInt();
}

bool newer(const String& remote, const String& local) {
    for (int i = 0; i < 3; ++i) {
        int r = versionPart(remote, i), l = versionPart(local, i);
        if (r > l) return true;
        if (r < l) return false;
    }
    return false;
}

String digestHex(const uint8_t d[32]) {
    const char* hex = "0123456789abcdef";
    String out; out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        out += hex[(d[i] >> 4) & 15];
        out += hex[d[i] & 15];
    }
    return out;
}

bool install(const String& url, size_t expectedSize, const String& expectedHash, String& error) {
    HTTPClient http;
    http.setConnectTimeout(RemoteConfig::HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(RemoteConfig::HTTP_REQUEST_TIMEOUT_MS);
    if (!http.begin(url)) { error = "Cannot initialize firmware request"; return false; }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        error = "Firmware HTTP " + String(code);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    size_t totalSize = contentLength > 0 ? (size_t)contentLength : expectedSize;
    if (!totalSize) { error = "Unknown firmware size"; http.end(); return false; }
    if (expectedSize && contentLength > 0 && expectedSize != totalSize) {
        error = "Firmware size differs from manifest";
        http.end(); return false;
    }

    if (!Update.begin(totalSize, U_FLASH)) {
        error = "Cannot open OTA partition";
        http.end(); return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts_ret(&sha, 0) != 0) {
        error = "SHA256 init failed";
        mbedtls_sha256_free(&sha);
        Update.abort(); http.end(); return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[4096];
    size_t total = 0;
    uint32_t lastData = millis();

    while (http.connected() && total < totalSize) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastData > RemoteConfig::HTTP_REQUEST_TIMEOUT_MS) {
                error = "Firmware download timed out";
                mbedtls_sha256_free(&sha);
                Update.abort(); http.end(); return false;
            }
            delay(2); continue;
        }

        size_t want = min(avail, sizeof(buf));
        int got = stream->readBytes(buf, want);
        if (got <= 0) continue;
        lastData = millis();

        if (mbedtls_sha256_update_ret(&sha, buf, got) != 0) {
            error = "SHA256 update failed";
            mbedtls_sha256_free(&sha);
            Update.abort(); http.end(); return false;
        }
        if (Update.write(buf, got) != (size_t)got) {
            error = "Flash write failed";
            mbedtls_sha256_free(&sha);
            Update.abort(); http.end(); return false;
        }

        total += got;
        Serial.printf("\rOTA %u/%u (%u%%)", (unsigned)total, (unsigned)totalSize,
                      (unsigned)((total * 100ULL) / totalSize));
    }
    Serial.println();

    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    http.end();

    if (total != totalSize) {
        error = "Incomplete firmware download";
        Update.abort(); return false;
    }

    String actual = digestHex(digest);
    if (!actual.equalsIgnoreCase(expectedHash)) {
        error = "SHA256 mismatch";
        Update.abort(); return false;
    }

    if (!Update.end(true)) {
        error = String("OTA finalize failed: ") + Update.errorString();
        return false;
    }
    return true;
}
}

namespace OtaClient {
void begin() {
    esp_ota_mark_app_valid_cancel_rollback();
}

Status check(bool installIfAvailable) {
    Status s;
    s.currentVersion = RemoteConfig::FIRMWARE_VERSION;

    if (WiFi.status() != WL_CONNECTED) {
        s.result = Result::NoWifi; s.message = "Wi-Fi is not connected"; return s;
    }

    HTTPClient http;
    String manifest = joinUrl(String(REMOTE_SERVER_URL), RemoteConfig::OTA_MANIFEST_PATH);
    http.setConnectTimeout(RemoteConfig::HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(RemoteConfig::HTTP_REQUEST_TIMEOUT_MS);

    if (!http.begin(manifest)) {
        s.message = "Cannot initialize manifest request"; return s;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        s.message = "Manifest HTTP " + String(code);
        http.end(); return s;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        s.message = "Manifest JSON parse failed"; return s;
    }

    String ver = doc["version"] | "";
    String path = doc["firmware_url"] | "";
    String hash = doc["sha256"] | "";
    size_t size = doc["size"] | 0;
    s.latestVersion = ver;

    if (ver.isEmpty() || path.isEmpty() || hash.length() != 64 || !size) {
        s.message = "Manifest missing required fields"; return s;
    }

    if (!newer(ver, s.currentVersion)) {
        s.result = Result::UpToDate;
        s.message = "Already current (" + s.currentVersion + ")";
        return s;
    }

    s.result = Result::UpdateAvailable;
    s.message = "Update available: " + s.currentVersion + " -> " + ver;
    if (!installIfAvailable) return s;

    String error;
    if (!install(joinUrl(String(REMOTE_SERVER_URL), path), size, hash, error)) {
        s.result = Result::InstallError; s.message = error; return s;
    }

    s.result = Result::Installed;
    s.message = "Installed " + ver + "; rebooting";
    Serial.println(s.message);
    delay(1000);
    ESP.restart();
    return s;
}
}
