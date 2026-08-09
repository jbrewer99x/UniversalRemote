#include <Arduino.h>
#include <SPI.h>

#include "display.h"
#include "config.h"

#define LCD_MOSI 45
#define LCD_SCLK 40
#define LCD_CS   42
#define LCD_DC   41
#define LCD_RST  39
#define LCD_BL   5

static SPIClass lcdSPI(FSPI);

static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 320;

static constexpr uint16_t COLOR_BLACK = 0x0000;
static constexpr uint16_t COLOR_WHITE = 0xFFFF;
static constexpr uint16_t COLOR_GREEN = 0x07E0;
static constexpr uint16_t COLOR_BLUE  = 0x001F;
static constexpr uint16_t COLOR_RED   = 0xF800;
static constexpr uint16_t COLOR_GRAY  = 0x8410;

static void lcdSelect() {
    digitalWrite(LCD_CS, LOW);
}

static void lcdDeselect() {
    digitalWrite(LCD_CS, HIGH);
}

static void writeCommand(uint8_t cmd) {
    digitalWrite(LCD_DC, LOW);
    lcdSelect();
    lcdSPI.transfer(cmd);
    lcdDeselect();
}

static void writeData(uint8_t data) {
    digitalWrite(LCD_DC, HIGH);
    lcdSelect();
    lcdSPI.transfer(data);
    lcdDeselect();
}

static void setAddressWindow(
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1
) {
    writeCommand(0x2A);

    digitalWrite(LCD_DC, HIGH);
    lcdSelect();
    lcdSPI.transfer(x0 >> 8);
    lcdSPI.transfer(x0 & 0xFF);
    lcdSPI.transfer(x1 >> 8);
    lcdSPI.transfer(x1 & 0xFF);
    lcdDeselect();

    writeCommand(0x2B);

    digitalWrite(LCD_DC, HIGH);
    lcdSelect();
    lcdSPI.transfer(y0 >> 8);
    lcdSPI.transfer(y0 & 0xFF);
    lcdSPI.transfer(y1 >> 8);
    lcdSPI.transfer(y1 & 0xFF);
    lcdDeselect();

    writeCommand(0x2C);
}

static void fillRect(
    uint16_t x,
    uint16_t y,
    uint16_t w,
    uint16_t h,
    uint16_t color
) {
    if (!w || !h) return;

    if (x >= SCREEN_W || y >= SCREEN_H) return;

    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;

    setAddressWindow(
        x,
        y,
        x + w - 1,
        y + h - 1
    );

    digitalWrite(LCD_DC, HIGH);
    lcdSelect();

    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;

    uint32_t count =
        static_cast<uint32_t>(w) *
        static_cast<uint32_t>(h);

    while (count--) {
        lcdSPI.transfer(hi);
        lcdSPI.transfer(lo);
    }

    lcdDeselect();
}

static void fillScreen(uint16_t color) {
    fillRect(0, 0, SCREEN_W, SCREEN_H, color);
}

// Minimal 5x7 font, enough for status/debug text.
static const uint8_t FONT[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
};

static void drawPixel(
    uint16_t x,
    uint16_t y,
    uint16_t color
) {
    if (x >= SCREEN_W || y >= SCREEN_H) return;

    setAddressWindow(x, y, x, y);

    digitalWrite(LCD_DC, HIGH);
    lcdSelect();
    lcdSPI.transfer(color >> 8);
    lcdSPI.transfer(color & 0xFF);
    lcdDeselect();
}

static const uint8_t* glyphFor(char c) {
    static uint8_t blank[5] = {0,0,0,0,0};

    if (c >= '0' && c <= '9') {
        return FONT[c - '0'];
    }

    return blank;
}

static void drawChar(
    uint16_t x,
    uint16_t y,
    char c,
    uint16_t color,
    uint8_t scale = 1
) {
    // Special-case a small set of common symbols/letters as block glyphs.
    if (c == ' ') return;

    const uint8_t* glyph = glyphFor(c);

    for (uint8_t col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];

        for (uint8_t row = 0; row < 7; ++row) {
            if (bits & (1 << row)) {
                fillRect(
                    x + col * scale,
                    y + row * scale,
                    scale,
                    scale,
                    color
                );
            }
        }
    }
}

static void drawText(
    uint16_t x,
    uint16_t y,
    const String& text,
    uint16_t color,
    uint8_t scale = 1
) {
    uint16_t cursorX = x;

    for (size_t i = 0; i < text.length(); ++i) {
        drawChar(cursorX, y, text[i], color, scale);
        cursorX += 6 * scale;
    }
}

void setDisplayBrightness(uint8_t percent) {
    if (percent > 100) percent = 100;

    static bool pwmReady = false;

    if (!pwmReady) {
        ledcSetup(0, 5000, 8);
        ledcAttachPin(LCD_BL, 0);
        pwmReady = true;
    }

    uint8_t duty = map(percent, 0, 100, 0, 255);
    ledcWrite(0, duty);
}

void initDisplay() {
    Serial.println("Display: starting raw ST7789 init");

    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_RST, OUTPUT);
    pinMode(LCD_BL, OUTPUT);

    digitalWrite(LCD_CS, HIGH);
    digitalWrite(LCD_BL, LOW);

    lcdSPI.begin(
        LCD_SCLK,
        -1,
        LCD_MOSI,
        LCD_CS
    );

    lcdSPI.beginTransaction(
        SPISettings(
            20000000,
            MSBFIRST,
            SPI_MODE0
        )
    );

    digitalWrite(LCD_RST, HIGH);
    delay(50);

    digitalWrite(LCD_RST, LOW);
    delay(100);

    digitalWrite(LCD_RST, HIGH);
    delay(150);

    writeCommand(0x01);
    delay(150);

    writeCommand(0x11);
    delay(120);

    writeCommand(0x3A);
    writeData(0x55);

    // Portrait rotation.
    writeCommand(0x36);
    writeData(0xC0);

    writeCommand(0x13);
    delay(10);

    writeCommand(0x29);
    delay(100);

    fillScreen(COLOR_BLACK);

    setDisplayBrightness(70);

    Serial.println("Display: ST7789 init complete");
}

void displayStatus(
    bool wifiConnected,
    const String &ipAddress,
    const String &otaStatus
) {
    fillScreen(COLOR_BLACK);

    // Header
    fillRect(0, 0, SCREEN_W, 36, COLOR_BLUE);

    // Simple visual blocks for now
    fillRect(
        12,
        52,
        wifiConnected ? 110 : 70,
        28,
        wifiConnected ? COLOR_GREEN : COLOR_RED
    );

    fillRect(
        12,
        96,
        200,
        22,
        COLOR_GRAY
    );

    fillRect(
        12,
        136,
        280,
        22,
        COLOR_GRAY
    );

    // For now, draw numeric portions only.
    drawText(
        18,
        15,
        String(RemoteConfig::FIRMWARE_VERSION),
        COLOR_WHITE,
        2
    );

    drawText(
        18,
        102,
        ipAddress,
        COLOR_WHITE,
        1
    );

    drawText(
        18,
        142,
        otaStatus,
        COLOR_WHITE,
        1
    );
}
