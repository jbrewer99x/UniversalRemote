#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <SD.h>
#include <mbedtls/sha256.h>
#include <vector>

#include "sd_updater.h"
#include "sd_storage.h"
#include "secrets.h"
#include "config.h"
#include "audio_player.h"
#include "sound_effects.h"
namespace {

static uint8_t sdIoBuffer[1024];

static Preferences sdPrefs;
static bool sdPrefsReady = false;

static constexpr const char* SD_PREF_NAMESPACE = "sd_update";
static constexpr const char* SD_PREF_MANIFEST_HASH = "manifest";


bool ensurePreferences() {
    if (sdPrefsReady) {
        return true;
    }

    sdPrefsReady = sdPrefs.begin(
        SD_PREF_NAMESPACE,
        false
    );

    if (!sdPrefsReady) {
        Serial.println(
            "SD Update: preferences unavailable"
        );
    }

    return sdPrefsReady;
}


String joinUrl(String base, const String& path) {
    if (
        path.startsWith("http://") ||
        path.startsWith("https://")
    ) {
        return path;
    }

    if (base.endsWith("/")) {
        base.remove(base.length() - 1);
    }

    if (!path.startsWith("/")) {
        base += "/";
    }

    base += path;
    return base;
}


String digestHex(const uint8_t digest[32]) {
    const char* hex = "0123456789abcdef";

    String result;
    result.reserve(64);

    for (int i = 0; i < 32; ++i) {
        result += hex[(digest[i] >> 4) & 0x0F];
        result += hex[digest[i] & 0x0F];
    }

    return result;
}


bool hashFile(
    const String& path,
    String& hash
) {
    File file = SD.open(
        path.c_str(),
        FILE_READ
    );

    if (!file) {
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (
        mbedtls_sha256_starts_ret(
            &sha,
            0
        ) != 0
    ) {
        file.close();
        mbedtls_sha256_free(&sha);
        return false;
    }

    while (file.available()) {
        size_t count = file.read(
            sdIoBuffer,
            sizeof(sdIoBuffer)
        );

        if (count == 0) {
            break;
        }

        if (
            mbedtls_sha256_update_ret(
                &sha,
                sdIoBuffer,
                count
            ) != 0
        ) {
            file.close();
            mbedtls_sha256_free(&sha);
            return false;
        }
    }

    uint8_t digest[32];

    if (
        mbedtls_sha256_finish_ret(
            &sha,
            digest
        ) != 0
    ) {
        file.close();
        mbedtls_sha256_free(&sha);
        return false;
    }

    mbedtls_sha256_free(&sha);
    file.close();

    hash = digestHex(digest);
    return true;
}


bool ensureParentDirectory(
    const String& path
) {
    int slash = path.lastIndexOf('/');

    if (slash <= 0) {
        return true;
    }

    String directory =
        path.substring(0, slash);

    if (SD.exists(directory.c_str())) {
        return true;
    }

    String current;
    int start = 1;

    while (start < directory.length()) {
        int next =
            directory.indexOf('/', start);

        if (next < 0) {
            next = directory.length();
        }

        current += "/";
        current += directory.substring(
            start,
            next
        );

        if (!SD.exists(current.c_str())) {
            if (!SD.mkdir(current.c_str())) {
                Serial.printf(
                    "SD Update: cannot create %s\n",
                    current.c_str()
                );

                return false;
            }
        }

        start = next + 1;
    }

    return true;
}


bool downloadFile(
    const String& url,
    const String& destination,
    const String& expectedHash
) {
    if (!ensureParentDirectory(destination)) {
        return false;
    }

    String temporary =
        destination + ".tmp";

    if (SD.exists(temporary.c_str())) {
        SD.remove(temporary.c_str());
    }

    HTTPClient http;

    http.setConnectTimeout(
        RemoteConfig::HTTP_CONNECT_TIMEOUT_MS
    );

    http.setTimeout(
        RemoteConfig::HTTP_REQUEST_TIMEOUT_MS
    );

    if (!http.begin(url)) {
        Serial.println(
            "SD Update: HTTP initialization failed"
        );

        return false;
    }

    int code = http.GET();

    if (code != HTTP_CODE_OK) {
        Serial.printf(
            "SD Update: HTTP %d for %s\n",
            code,
            url.c_str()
        );

        http.end();
        return false;
    }

    File file = SD.open(
        temporary.c_str(),
        FILE_WRITE
    );

    if (!file) {
        Serial.printf(
            "SD Update: cannot open %s\n",
            temporary.c_str()
        );

        http.end();
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (
        mbedtls_sha256_starts_ret(
            &sha,
            0
        ) != 0
    ) {
        Serial.println(
            "SD Update: SHA256 initialization failed"
        );

        file.close();
        SD.remove(temporary.c_str());
        http.end();
        mbedtls_sha256_free(&sha);

        return false;
    }

    WiFiClient* stream =
        http.getStreamPtr();

    int contentLength =
        http.getSize();

    size_t total = 0;
    uint32_t lastData = millis();

    while (
        http.connected() &&
        (
            contentLength < 0 ||
            total < (size_t)contentLength
        )
    ) {
        size_t available =
            stream->available();

        if (!available) {
            if (
                millis() - lastData >
                RemoteConfig::HTTP_REQUEST_TIMEOUT_MS
            ) {
                Serial.println(
                    "SD Update: download timed out"
                );

                file.close();
                SD.remove(temporary.c_str());
                http.end();
                mbedtls_sha256_free(&sha);

                return false;
            }

            delay(2);
            continue;
        }

        size_t wanted = min(
            available,
            sizeof(sdIoBuffer)
        );

        int received =
            stream->readBytes(
                sdIoBuffer,
                wanted
            );

        if (received <= 0) {
            continue;
        }

        lastData = millis();

        if (
            file.write(
                sdIoBuffer,
                received
            ) != (size_t)received
        ) {
            Serial.println(
                "SD Update: SD write failed"
            );

            file.close();
            SD.remove(temporary.c_str());
            http.end();
            mbedtls_sha256_free(&sha);

            return false;
        }

        if (
            mbedtls_sha256_update_ret(
                &sha,
                sdIoBuffer,
                received
            ) != 0
        ) {
            Serial.println(
                "SD Update: SHA256 update failed"
            );

            file.close();
            SD.remove(temporary.c_str());
            http.end();
            mbedtls_sha256_free(&sha);

            return false;
        }

        total += received;

        if (contentLength > 0) {
            Serial.printf(
                "\rSD Update: %u/%u (%u%%)",
                (unsigned)total,
                (unsigned)contentLength,
                (unsigned)(
                    (total * 100ULL) /
                    contentLength
                )
            );
        }
    }

    Serial.println();

    uint8_t digest[32];

    if (
        mbedtls_sha256_finish_ret(
            &sha,
            digest
        ) != 0
    ) {
        Serial.println(
            "SD Update: SHA256 finalize failed"
        );

        file.close();
        SD.remove(temporary.c_str());
        http.end();
        mbedtls_sha256_free(&sha);

        return false;
    }

    mbedtls_sha256_free(&sha);
    file.close();
    http.end();

    if (
        contentLength >= 0 &&
        total != (size_t)contentLength
    ) {
        Serial.printf(
            "SD Update: incomplete download for %s\n",
            destination.c_str()
        );

        SD.remove(temporary.c_str());
        return false;
    }

    String actualHash =
        digestHex(digest);

    if (
        !actualHash.equalsIgnoreCase(
            expectedHash
        )
    ) {
        Serial.printf(
            "SD Update: SHA256 mismatch for %s\n",
            destination.c_str()
        );

        SD.remove(temporary.c_str());
        return false;
    }

    if (SD.exists(destination.c_str())) {
        if (!SD.remove(destination.c_str())) {
            Serial.printf(
                "SD Update: cannot replace %s\n",
                destination.c_str()
            );

            SD.remove(temporary.c_str());
            return false;
        }
    }

    if (
        !SD.rename(
            temporary.c_str(),
            destination.c_str()
        )
    ) {
        Serial.printf(
            "SD Update: rename failed for %s\n",
            destination.c_str()
        );

        SD.remove(temporary.c_str());
        return false;
    }

    Serial.printf(
        "SD Update: installed %s (%u bytes)\n",
        destination.c_str(),
        (unsigned)total
    );

    return true;
}


bool pathIsExpected(
    const String& path,
    const std::vector<String>& expected
) {
    for (const String& item : expected) {
        if (item == path) {
            return true;
        }
    }

    return false;
}


void cleanManagedDirectory(
    const String& directory,
    const std::vector<String>& expected
) {
    File root = SD.open(directory.c_str());

    if (!root || !root.isDirectory()) {
        return;
    }

    File entry =
        root.openNextFile();

    while (entry) {
        String path =
            entry.path();

        if (entry.isDirectory()) {
            entry.close();

            cleanManagedDirectory(
                path,
                expected
            );

            File check =
                SD.open(path.c_str());

            if (
                check &&
                check.isDirectory()
            ) {
                File child =
                    check.openNextFile();

                if (!child) {
                    check.close();

                    if (SD.rmdir(path.c_str())) {
                        Serial.printf(
                            "SD Update: removed directory %s\n",
                            path.c_str()
                        );
                    }
                }
                else {
                    child.close();
                    check.close();
                }
            }
        }
        else {
            entry.close();

            if (
                !pathIsExpected(
                    path,
                    expected
                )
            ) {
                if (SD.remove(path.c_str())) {
                    Serial.printf(
                        "SD Update: removed stale %s\n",
                        path.c_str()
                    );
                }
            }
        }

        entry =
            root.openNextFile();
    }

    root.close();
}


bool isManagedPathSafe(
    const String& path
) {
    if (!path.startsWith("/content/")) {
        return false;
    }

    if (
        path.indexOf("/../") >= 0 ||
        path.endsWith("/..") ||
        path.indexOf("/./") >= 0 ||
        path.endsWith("/.")
    ) {
        return false;
    }

    return true;
}

} // namespace


namespace SdUpdater {

bool check() {
    if (!isSdCardReady()) {
        Serial.println(
            "SD Update: card unavailable; no changes made"
        );

        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(
            "SD Update: Wi-Fi unavailable; no changes made"
        );

        return false;
    }

    Serial.println(
        "SD Update: checking manifest"
    );

    String manifestUrl =
        joinUrl(
            String(REMOTE_SERVER_URL),
            "/api/firmware/sd/manifest"
        );

    HTTPClient http;

    http.setConnectTimeout(
        RemoteConfig::HTTP_CONNECT_TIMEOUT_MS
    );

    http.setTimeout(
        RemoteConfig::HTTP_REQUEST_TIMEOUT_MS
    );

    if (!http.begin(manifestUrl)) {
        Serial.println(
            "SD Update: manifest request failed; no changes made"
        );

        return false;
    }

    int code =
        http.GET();

    if (code != HTTP_CODE_OK) {
        Serial.printf(
            "SD Update: manifest HTTP %d; no changes made\n",
            code
        );

        http.end();
        return false;
    }

    String body =
        http.getString();

    http.end();

    if (body.isEmpty()) {
        Serial.println(
            "SD Update: empty manifest response; no changes made"
        );

        return false;
    }

    JsonDocument doc;

    DeserializationError jsonError =
        deserializeJson(
            doc,
            body
        );

    if (jsonError) {
        Serial.printf(
            "SD Update: manifest JSON invalid (%s); no changes made\n",
            jsonError.c_str()
        );

        return false;
    }

    bool manifestValid =
        doc["valid"] | false;

    String remoteManifestHash =
        doc["manifest_sha256"] | "";

    JsonArray files =
        doc["files"].as<JsonArray>();

    if (
        !manifestValid ||
        remoteManifestHash.length() != 64 ||
        files.isNull()
    ) {
        Serial.println(
            "SD Update: manifest missing validity/hash/files; no changes made"
        );

        return false;
    }

    // Fast path:
    // If the server's complete content hash matches the last successfully
    // applied hash, nothing on the managed SD content has changed. Do not open
    // or hash any SD files.
    if (ensurePreferences()) {
        String localManifestHash =
            sdPrefs.getString(
                SD_PREF_MANIFEST_HASH,
                ""
            );

        if (
            localManifestHash.length() == 64 &&
            localManifestHash.equalsIgnoreCase(
                remoteManifestHash
            )
        ) {
            Serial.printf(
                "SD Update: content current (%u file(s)); fast check complete\n",
                (unsigned)files.size()
            );

            return true;
        }
    }

    // The manifest hash changed (or this is the first sync). Validate every
    // manifest entry before modifying or deleting anything.
    std::vector<String> expectedFiles;
    expectedFiles.reserve(files.size());

    for (JsonObject item : files) {
        String path =
            item["path"] | "";

        String url =
            item["url"] | "";

        String expectedHash =
            item["sha256"] | "";

        if (
            path.isEmpty() ||
            url.isEmpty() ||
            expectedHash.length() != 64
        ) {
            Serial.println(
                "SD Update: invalid manifest entry; no changes made"
            );

            return false;
        }

        if (!path.startsWith("/")) {
            path =
                "/" + path;
        }

        if (!isManagedPathSafe(path)) {
            Serial.printf(
                "SD Update: unsafe manifest path %s; no changes made\n",
                path.c_str()
            );

            return false;
        }

        expectedFiles.push_back(path);
    }

    Serial.printf(
        "SD Update: content changed; validating %u file(s)\n",
        (unsigned)expectedFiles.size()
    );

    unsigned checked = 0;
    unsigned updated = 0;
    bool syncComplete = true;

    for (JsonObject item : files) {
        String path =
            item["path"] | "";

        String url =
            item["url"] | "";

        String expectedHash =
            item["sha256"] | "";

        if (!path.startsWith("/")) {
            path =
                "/" + path;
        }

        checked++;

        bool needsUpdate = true;

        if (SD.exists(path.c_str())) {
            String existingHash;

            if (
                hashFile(
                    path,
                    existingHash
                ) &&
                existingHash.equalsIgnoreCase(
                    expectedHash
                )
            ) {
                needsUpdate = false;
            }
        }

        if (!needsUpdate) {
            Serial.printf(
                "SD Update: current %s\n",
                path.c_str()
            );

            continue;
        }

        Serial.printf(
            "SD Update: downloading %s\n",
            path.c_str()
        );

        if (
            downloadFile(
                joinUrl(
                    String(REMOTE_SERVER_URL),
                    url
                ),
                path,
                expectedHash
            )
        ) {
            updated++;
        }
        else {
            syncComplete = false;

            Serial.printf(
                "SD Update: failed updating %s\n",
                path.c_str()
            );
        }
    }

    if (!syncComplete) {
        Serial.println(
            "SD Update: sync incomplete; stale-file cleanup skipped"
        );

        Serial.printf(
            "SD Update: checked %u, updated %u\n",
            checked,
            updated
        );
        if (updated > 0) {
            playSoundEffect(
                SoundEffect::NewFilesLoaded
    );
}
        return false;
    }

    // Only a fully valid and fully successful sync is authoritative enough to
    // remove stale managed files.
    if (SD.exists("/content")) {
        Serial.println(
            "SD Update: sync complete; checking stale files"
        );

        cleanManagedDirectory(
            "/content",
            expectedFiles
        );
    }

    // Persist the server manifest hash ONLY after all updates and cleanup
    // completed successfully. A failed/partial sync never marks itself current.
    if (ensurePreferences()) {
        size_t saved =
            sdPrefs.putString(
                SD_PREF_MANIFEST_HASH,
                remoteManifestHash
            );

        if (saved == 0) {
            Serial.println(
                "SD Update: warning - could not save manifest hash; next check will fully verify files"
            );
        }
        else {
            Serial.println(
                "SD Update: saved new content manifest hash"
            );
        }
    }

    Serial.printf(
        "SD Update: checked %u, updated %u\n",
        checked,
        updated
    );

    return true;
}

} // namespace SdUpdater
