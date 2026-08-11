#include <Arduino.h>
#include <SPI.h>
#include "font5x7.h"
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
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }

    return &FONT5X7[(c - 0x20) * 5];
}

static void drawChar(
    uint16_t x,
    uint16_t y,
    char c,
    uint16_t color,
    uint8_t scale = 1
) {
    const uint8_t* glyph = glyphFor(c);

    for (uint8_t col = 0; col < 5; ++col) {
        uint8_t bits = pgm_read_byte(&glyph[col]);

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
    writeData(0xC8);

    writeCommand(0x13);
    delay(10);

    // Display inversion on
    writeCommand(0x21);
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

    // ---------- STATUS BAR ----------
    fillRect(0, 0, SCREEN_W, 28, 0x2104);

    // Wi-Fi indicator
    fillRect(
        10,
        10,
        8,
        8,
        wifiConnected ? COLOR_GREEN : COLOR_RED
    );

    // Battery percentage is drawn separately by
    // updateBatteryStatus() from main.cpp.


    // ---------- DEVICE SELECTOR ----------
    // PC selected by default for this visual test.
    fillRect(8, 36, 108, 30, COLOR_GREEN);
    drawText(55, 47, "PC", COLOR_BLACK, 1);

    fillRect(124, 36, 108, 30, 0x18E3);
    drawText(164, 47, "ROKU", COLOR_WHITE, 1);


    // ---------- NAVIGATION ----------
    // Power / Home / Back
    fillRect(8, 74, 70, 32, 0x18E3);
    drawText(25, 86, "Power", COLOR_RED, 1);

    fillRect(85, 74, 70, 32, 0x18E3);
    drawText(105, 86, "Home", COLOR_WHITE, 1);

    fillRect(162, 74, 70, 32, 0x18E3);
    drawText(183, 86, "Back", COLOR_WHITE, 1);


    // ---------- D-PAD ----------
    // Up
    fillRect(88, 113, 64, 34, 0x18E3);
    drawText(117, 125, "^", COLOR_WHITE, 1);

    // Left
    fillRect(17, 151, 64, 38, 0x18E3);
    drawText(47, 165, "<", COLOR_WHITE, 1);

    // OK
    fillRect(88, 151, 64, 38, COLOR_GREEN);
    drawText(113, 165, "OK", COLOR_BLACK, 1);

    // Right
    fillRect(159, 151, 64, 38, 0x18E3);
    drawText(188, 165, ">", COLOR_WHITE, 1);

    // Down
    fillRect(88, 193, 64, 34, 0x18E3);
    drawText(117, 205, "v", COLOR_WHITE, 1);


    // ---------- MEDIA ----------
    fillRect(8, 235, 70, 32, 0x18E3);
    drawText(28, 247, "|<<", COLOR_WHITE, 1);

    fillRect(85, 235, 70, 32, 0x18E3);
    drawText(105, 247, ">||", COLOR_WHITE, 1);

    fillRect(162, 235, 70, 32, 0x18E3);
    drawText(182, 247, ">>|", COLOR_WHITE, 1);


    // ---------- AUDIO ----------
    fillRect(8, 275, 70, 36, 0x18E3);
    drawText(26, 289, "Vol-", COLOR_WHITE, 1);

    fillRect(85, 275, 70, 36, 0x18E3);
    drawText(105, 289, "Mute", COLOR_WHITE, 1);

    fillRect(162, 275, 70, 36, 0x18E3);
    drawText(180, 289, "Vol+", COLOR_WHITE, 1);
}
void displaySettings(
    uint8_t brightnessPercent,
    uint16_t sleepSeconds
) {
    fillScreen(COLOR_BLACK);

    // Header
    fillRect(0, 0, SCREEN_W, 36, 0x2104);

    drawText(10, 14, "< Back", COLOR_WHITE, 1);
    drawText(92, 14, "Settings", COLOR_WHITE, 1);

    // Brightness
    drawText(14, 60, "Brightness", COLOR_WHITE, 1);

    String brightnessText =
        String(brightnessPercent) + "%";

    drawText(190, 60, brightnessText, 0xC618, 1);

    // Brightness slider background
    fillRect(14, 82, 212, 8, 0x4208);

    uint16_t brightnessWidth =
        ((uint32_t)brightnessPercent * 212) / 100;

    fillRect(
        14,
        82,
        brightnessWidth,
        8,
        COLOR_WHITE
    );

    // Slider thumb
    uint16_t brightnessX =
        14 + ((uint32_t)brightnessPercent * 212) / 100;

    if (brightnessX > 225) {
        brightnessX = 225;
    }

    fillRect(
        brightnessX - 3,
        77,
        7,
        18,
        COLOR_WHITE
    );

    // Sleep timer
    drawText(14, 126, "Sleep Timer", COLOR_WHITE, 1);

    String sleepText;

    if (sleepSeconds == 0) {
        sleepText = "Off";
    } else {
        sleepText = String(sleepSeconds) + " sec";
    }

    drawText(170, 126, sleepText, 0xC618, 1);

    // Sleep slider
    fillRect(14, 150, 212, 8, 0x4208);

    uint16_t clampedSleep =
        constrain(sleepSeconds, 2, 120);

    uint16_t sleepWidth =
        ((uint32_t)(clampedSleep - 2) * 212) / 118;

    fillRect(
        14,
        150,
        sleepWidth,
        8,
        COLOR_WHITE
    );

    uint16_t sleepX = 14 + sleepWidth;

    if (sleepX > 225) {
        sleepX = 225;
    }

    fillRect(
        sleepX - 3,
        145,
        7,
        18,
        COLOR_WHITE
    );

    // Future settings
    drawText(14, 198, "WiFi", 0x8410, 1);
    drawText(14, 220, "Bluetooth", 0x8410, 1);

    drawText(
        14,
        286,
        "Universal Remote 0.1.7",
        0x8410,
        1
    );
}
void updateBrightnessSlider(uint8_t brightnessPercent) {
    // Clear only the dynamic brightness area
    fillRect(190, 60, 36, 8, COLOR_BLACK);
    fillRect(14, 77, 212, 18, COLOR_BLACK);

    drawText(14, 60, "Brightness", COLOR_WHITE, 1);

    String valueText = String(brightnessPercent) + "%";
    drawText(190, 60, valueText, 0xC618, 1);

    // Slider track
    fillRect(14, 82, 212, 8, 0x4208);

    uint16_t width =
        ((uint32_t)brightnessPercent * 212) / 100;

    if (width > 0) {
        fillRect(14, 82, width, 8, COLOR_WHITE);
    }

    uint16_t thumbX = 14 + width;

    if (thumbX > 225) {
        thumbX = 225;
    }

    fillRect(
        thumbX - 3,
        77,
        7,
        18,
        COLOR_WHITE
    );
}

void updateSleepSlider(uint16_t sleepSeconds) {
    static uint16_t lastThumbX = 14;

    uint16_t value = constrain(
        sleepSeconds,
        2,
        120
    );

    uint16_t width =
        ((uint32_t)(value - 2) * 212) / 118;

    uint16_t thumbX = 14 + width;

    if (thumbX > 225) {
        thumbX = 225;
    }

    // Update numeric value only
    fillRect(170, 126, 56, 8, COLOR_BLACK);

    String valueText =
        String(sleepSeconds) + " sec";

    drawText(
        170,
        126,
        valueText,
        0xC618,
        1
    );

    // Erase old thumb area only
    fillRect(
        lastThumbX - 4,
        144,
        9,
        20,
        COLOR_BLACK
    );

    // Redraw track
    fillRect(
        14,
        150,
        212,
        8,
        0x4208
    );

    // Active portion
    if (width > 0) {
        fillRect(
            14,
            150,
            width,
            8,
            COLOR_WHITE
        );
    }

    // New thumb
    fillRect(
        thumbX - 3,
        145,
        7,
        18,
        COLOR_WHITE
    );

    lastThumbX = thumbX;
    
}

void updateBatteryStatus(uint8_t percent) {
    if (percent > 100) percent = 100;

    // Clear only the battery portion of the status bar.
    fillRect(196, 4, 44, 20, 0x2104);

    String text = String(percent) + "%";

    drawText(
        202,
        9,
        text,
        COLOR_WHITE,
        1
    );
}