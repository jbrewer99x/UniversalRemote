#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#include "sd_storage.h"

#define SD_MISO 16
#define SD_MOSI 17
#define SD_SCK  14
#define SD_CS   21

static SPIClass sdSpi = SPIClass(HSPI);
static bool sdReady = false;

bool initSdCard() {
    Serial.println("SD: initializing");

    sdSpi.begin(
        SD_SCK,
        SD_MISO,
        SD_MOSI,
        SD_CS
    );

    if (!SD.begin(SD_CS, sdSpi)) {
        Serial.println("SD: mount FAILED");
        sdReady = false;
        return false;
    }

    uint8_t type = SD.cardType();

    if (type == CARD_NONE) {
        Serial.println("SD: no card detected");
        sdReady = false;
        return false;
    }

    Serial.printf(
        "SD: mounted, size=%llu MB\n",
        SD.cardSize() / (1024ULL * 1024ULL)
    );

    sdReady = true;
    return true;
}

bool writeSdTextFile(
    const char* path,
    const String& text
) {
    if (!sdReady) return false;

    File file = SD.open(path, FILE_WRITE);

    if (!file) {
        Serial.printf(
            "SD: failed opening %s for write\n",
            path
        );
        return false;
    }

    file.print(text);
    file.close();

    return true;
}

String readSdTextFile(const char* path) {
    if (!sdReady) return "";

    File file = SD.open(path, FILE_READ);

    if (!file) {
        Serial.printf(
            "SD: failed opening %s for read\n",
            path
        );
        return "";
    }

    String result;

    while (file.available()) {
        result += (char)file.read();
    }

    file.close();

    return result;
}

bool testSdCard() {
    const char* path = "/sd_test.txt";

    String expected =
        "Universal Remote SD test\n";

    if (!writeSdTextFile(path, expected)) {
        return false;
    }

    String actual =
        readSdTextFile(path);

    if (actual != expected) {
        Serial.println(
            "SD: read/write verification FAILED"
        );
        return false;
    }

    Serial.println(
        "SD: read/write verification OK"
    );

    return true;
}
bool isSdCardReady() {
    return sdReady;
}