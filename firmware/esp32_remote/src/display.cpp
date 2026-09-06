#include <Arduino.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "display.h"
#include "config.h"
#include "power_manager.h"

namespace {
constexpr int LCD_MOSI = 45, LCD_SCLK = 40, LCD_CS = 42;
constexpr int LCD_DC = 41, LCD_RST = 39, LCD_BL = 5;
constexpr int SCREEN_W = 240, SCREEN_H = 320;
constexpr uint32_t BACKGROUND = 0x10151C, SURFACE = 0x252E3A;
constexpr uint32_t TEXT = 0xEDF2F7, MUTED = 0xA3B2C2, ACCENT = 0x67DF9A;
SPIClass lcdSPI(FSPI);
lv_display_t* display = nullptr;
lv_indev_t* input = nullptr;
lv_obj_t *home, *settings, *lights, *wifi, *battery, *pc, *roku;
int8_t displayedWifiLevel = -1;
lv_obj_t *brightnessSlider, *sleepSlider, *brightnessValue, *sleepValue;
lv_obj_t *lightBrightnessSlider, *lightTemperatureSlider, *lightBrightnessValue, *lightTemperatureValue;
lv_obj_t *updateMessage, *feedback;
bool ready = false, touchDown = false;
bool panelSleeping = false, sleepRequested = false;
uint32_t lastSleepOutAt = 0;
uint64_t totalFlushUs = 0;
uint16_t touchX = 0, touchY = 0;
uint32_t feedbackUntil = 0, maxServiceUs = 0, maxFlushUs = 0;
DisplayAction actions[16];
uint8_t actionHead = 0, actionCount = 0;

void writeCommand(uint8_t command) {
    digitalWrite(LCD_DC, LOW);
    digitalWrite(LCD_CS, LOW);
    lcdSPI.transfer(command);
    digitalWrite(LCD_CS, HIGH);
}
void writeData(uint8_t data) {
    digitalWrite(LCD_DC, HIGH);
    digitalWrite(LCD_CS, LOW);
    lcdSPI.transfer(data);
    digitalWrite(LCD_CS, HIGH);
}
void addressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    writeCommand(0x2A);
    uint8_t columns[] = {uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)};
    digitalWrite(LCD_DC, HIGH);
    digitalWrite(LCD_CS, LOW);
    lcdSPI.writeBytes(columns, sizeof(columns));
    digitalWrite(LCD_CS, HIGH);
    writeCommand(0x2B);
    uint8_t rows[] = {uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)};
    digitalWrite(LCD_DC, HIGH);
    digitalWrite(LCD_CS, LOW);
    lcdSPI.writeBytes(rows, sizeof(rows));
    digitalWrite(LCD_CS, HIGH);
    writeCommand(0x2C);
}
void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* pixels) {
    const uint32_t started = micros();
    const size_t count = lv_area_get_width(area) * lv_area_get_height(area);
    // LVGL renders native little-endian RGB565; ST7789 accepts MSB first.
    lv_draw_sw_rgb565_swap(pixels, count);
    lcdSPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
    addressWindow(area->x1, area->y1, area->x2, area->y2);
    digitalWrite(LCD_DC, HIGH);
    digitalWrite(LCD_CS, LOW);
    lcdSPI.writeBytes(pixels, count * 2);
    digitalWrite(LCD_CS, HIGH);
    lcdSPI.endTransaction();
    const uint32_t elapsed = micros() - started;
    maxFlushUs = max(maxFlushUs, elapsed);
    totalFlushUs += elapsed;
    lv_display_flush_ready(disp);
}
void readInput(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = touchX;
    data->point.y = touchY;
    data->state = touchDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
void enqueue(const char* name, int value = 0) {
    if (actionCount == 16) { displayFeedback("Input busy. Try again."); return; }
    actions[(actionHead + actionCount) % 16] = {name, value};
    ++actionCount;
}
void buttonEvent(lv_event_t* event) {
    enqueue(static_cast<const char*>(lv_event_get_user_data(event)));
}
void sliderEvent(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    const char* name = static_cast<const char*>(lv_event_get_user_data(event));
    if (code == LV_EVENT_VALUE_CHANGED) {
        auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
        const int value = lv_slider_get_value(slider);
        enqueue(name, value);
        if (!strcmp(name, "govee_brightness")) lv_label_set_text_fmt(lightBrightnessValue, "%d%%", value);
        else if (!strcmp(name, "govee_temperature")) lv_label_set_text_fmt(lightTemperatureValue, "%dK", value);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) enqueue("save_settings");
}
lv_obj_t* label(lv_obj_t* parent, const char* text, int x, int y, int width,
                const lv_font_t* font = &lv_font_montserrat_12) {
    auto* obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(TEXT), 0);
    lv_label_set_text(obj, text);
    return obj;
}
lv_obj_t* button(lv_obj_t* parent, int x, int y, int w, int h,
                 const char* text, const char* action) {
    auto* obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(SURFACE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x354353), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x466253), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_hex(ACCENT), LV_STATE_PRESSED);
    if (text && text[0] != '\0') {
        auto* caption = label(obj, text, 0, 0, w - 4);
        lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(caption);
    }
    lv_obj_add_event_cb(obj, buttonEvent, LV_EVENT_PRESSED, const_cast<char*>(action));
    return obj;
}
lv_obj_t* screen() {
    auto* obj = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(obj, lv_color_hex(BACKGROUND), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
lv_obj_t* slider(lv_obj_t* parent, int y, int minimum, int maximum, const char* action) {
    auto* obj = lv_slider_create(parent);
    lv_obj_set_pos(obj, 14, y);
    lv_obj_set_size(obj, 212, 8);
    lv_slider_set_range(obj, minimum, maximum);
    lv_obj_set_ext_click_area(obj, 14);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x354353), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(obj, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(obj, sliderEvent, LV_EVENT_ALL, const_cast<char*>(action));
    return obj;
}
// One small draw object: no bitmaps, extra widgets, or animation timer.
void drawWifiIcon(lv_event_t* event) {
    lv_area_t area;
    lv_obj_get_coords(wifi, &area);
    auto* layer = lv_event_get_layer(event);
    if (displayedWifiLevel <= 0) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_hex(0xFF4040);
        line.width = 2;
        line.round_start = line.round_end = 1;
        line.p1 = {area.x1 + 5, area.y1 + 3};
        line.p2 = {area.x1 + 17, area.y1 + 15};
        lv_draw_line(layer, &line);
        line.p1.y = area.y1 + 15;
        line.p2.y = area.y1 + 3;
        lv_draw_line(layer, &line);
        return;
    }
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center = {area.x1 + 11, area.y1 + 16};
    arc.start_angle = 225;
    arc.end_angle = 315;
    arc.width = 2;
    arc.rounded = 1;
    for (int i = 0; i < 3; ++i) {
        arc.radius = 6 + i * 4;
        arc.color = lv_color_hex(i < displayedWifiLevel ? ACCENT : 0x354353);
        lv_draw_arc(layer, &arc);
    }
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_hex(ACCENT);
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t dotArea = {area.x1 + 10, area.y1 + 15, area.x1 + 12, area.y1 + 17};
    lv_draw_rect(layer, &dot, &dotArea);
}
void createScreens() {
    home = screen();
    auto* header = button(home, 0, 0, 240, 28, "", "settings");
    lv_obj_set_style_radius(header, 0, 0);
    wifi = lv_obj_create(header);
    lv_obj_remove_style_all(wifi);
    lv_obj_remove_flag(wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(wifi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(wifi, 10, 4);
    lv_obj_set_size(wifi, 23, 19);
    lv_obj_add_event_cb(wifi, drawWifiIcon, LV_EVENT_DRAW_MAIN, nullptr);
    label(header, LV_SYMBOL_SETTINGS, 110, 6, 20);
    battery = label(header, "--.--V", 136, 6, 94, &lv_font_montserrat_12);
    lv_obj_set_style_text_align(battery, LV_TEXT_ALIGN_RIGHT, 0);
    pc = button(home, 8, 36, 108, 30, "PC", "pc");
    roku = button(home, 124, 36, 108, 30, "Roku", "roku");
    auto* power = button(home, 8, 74, 70, 32, LV_SYMBOL_POWER " Power", "power");
    lv_obj_set_style_text_color(lv_obj_get_child(power, 0), lv_color_hex(0xFF9393), 0);
    button(home, 85, 74, 70, 32, LV_SYMBOL_HOME " Home", "home");
    button(home, 162, 74, 70, 32, LV_SYMBOL_LEFT " Back", "back");
    button(home, 88, 113, 64, 34, LV_SYMBOL_UP, "up");
    button(home, 17, 151, 64, 38, LV_SYMBOL_LEFT, "left");
    auto* ok = button(home, 88, 151, 64, 38, "OK", "ok");
    lv_obj_set_style_bg_color(ok, lv_color_hex(ACCENT), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(ok, 0), lv_color_hex(BACKGROUND), 0);
    button(home, 159, 151, 64, 38, LV_SYMBOL_RIGHT, "right");
    button(home, 17, 193, 64, 34, LV_SYMBOL_PREV " Prev", "previous");
    button(home, 88, 193, 64, 34, LV_SYMBOL_DOWN, "down");
    button(home, 159, 193, 64, 34, "Next " LV_SYMBOL_NEXT, "next");
    button(home, 8, 235, 70, 32, LV_SYMBOL_LEFT LV_SYMBOL_LEFT, "rewind");
    button(home, 85, 235, 70, 32, LV_SYMBOL_PLAY "  " LV_SYMBOL_PAUSE, "play_pause");
    button(home, 162, 235, 70, 32, LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT, "fast_forward");
    button(home, 8, 275, 70, 36, LV_SYMBOL_VOLUME_MID " -", "volume_down");
    button(home, 85, 275, 70, 36, "Mute", "mute");
    button(home, 162, 275, 70, 36, LV_SYMBOL_VOLUME_MAX " +", "volume_up");

    settings = screen();
    button(settings, 0, 0, 75, 36, LV_SYMBOL_LEFT " Back", "show_home");
    label(settings, "Settings", 92, 10, 130, &lv_font_montserrat_14);
    label(settings, "Brightness", 14, 56, 145);
    brightnessValue = label(settings, "", 175, 56, 51);
    lv_obj_set_style_text_align(brightnessValue, LV_TEXT_ALIGN_RIGHT, 0);
    brightnessSlider = slider(settings, 82, 0, 100, "brightness");
    label(settings, "Sleep Timer", 14, 122, 145);
    sleepValue = label(settings, "", 160, 122, 66);
    lv_obj_set_style_text_align(sleepValue, LV_TEXT_ALIGN_RIGHT, 0);
    sleepSlider = slider(settings, 150, 2, 120, "sleep");
    updateMessage = label(settings, "", 14, 182, 212, &lv_font_montserrat_10);
    lv_obj_set_height(updateMessage, 28);
    lv_label_set_long_mode(updateMessage, LV_LABEL_LONG_DOT);
    button(settings, 20, 215, 200, 40, LV_SYMBOL_DOWNLOAD " Check for updates", "updates");
    button(settings, 20, 265, 200, 40, LV_SYMBOL_AUDIO " Play Sounds", "sounds");
    auto* footer = label(settings, "Universal Remote " , 14, 308, 226, &lv_font_montserrat_10);
    lv_label_set_text_fmt(footer, "Universal Remote %s", RemoteConfig::FIRMWARE_VERSION);
    lv_obj_set_style_text_color(footer, lv_color_hex(MUTED), 0);

    lights = screen();
    button(lights, 0, 0, 74, 36, LV_SYMBOL_LEFT " Back", "show_home");
    label(lights, "Govee Lights", 95, 10, 145, &lv_font_montserrat_14);
    button(lights, 10, 46, 100, 36, LV_SYMBOL_POWER " On", "govee_on");
    button(lights, 130, 46, 100, 36, LV_SYMBOL_POWER " Off", "govee_off");
    label(lights, "Brightness", 14, 96, 120);
    lightBrightnessValue = label(lights, "50%", 180, 96, 46);
    lv_obj_set_style_text_align(lightBrightnessValue, LV_TEXT_ALIGN_RIGHT, 0);
    lightBrightnessSlider = slider(lights, 122, 0, 100, "govee_brightness");
    label(lights, "Temp", 14, 156, 70);
    lightTemperatureValue = label(lights, "4000K", 160, 156, 66);
    lv_obj_set_style_text_align(lightTemperatureValue, LV_TEXT_ALIGN_RIGHT, 0);
    lightTemperatureSlider = slider(lights, 182, 1000, 10000, "govee_temperature");
    button(lights, 16, 214, 68, 34, "Warm", "govee_warm");
    button(lights, 88, 214, 68, 34, "Cool", "govee_cool");
    button(lights, 160, 214, 64, 34, "Red", "govee_red");
    button(lights, 16, 254, 68, 34, "Green", "govee_green");
    button(lights, 88, 254, 68, 34, "Blue", "govee_blue");
    button(lights, 160, 254, 64, 34, "Party", "govee_party");
    auto* crazy = button(lights, 16, 288, 208, 26, LV_SYMBOL_SHUFFLE " Crazy Mode", "govee_crazy_toggle");
    lv_obj_set_style_bg_color(crazy, lv_color_hex(0x466253), 0);
    lv_obj_set_style_border_color(crazy, lv_color_hex(ACCENT), 0);

    feedback = label(lv_layer_top(), "", 4, 2, 232, &lv_font_montserrat_12);
    lv_obj_set_style_bg_color(feedback, lv_color_hex(SURFACE), 0);
    lv_obj_set_style_bg_opa(feedback, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(feedback, 5, 0);
    lv_obj_add_flag(feedback, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(home);
}
}

bool initDisplay() {
    pinMode(LCD_CS, OUTPUT); pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_RST, OUTPUT); pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_CS, HIGH); digitalWrite(LCD_BL, LOW);
    // Fail before any LVGL object is created. Power/button/audio remain serviceable.
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(240 * 32 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buffer) { Serial.println("Display: cannot allocate 15360-byte render buffer"); return false; }
    lcdSPI.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);
    lcdSPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
    digitalWrite(LCD_RST, HIGH); delay(50);
    digitalWrite(LCD_RST, LOW); delay(100);
    digitalWrite(LCD_RST, HIGH); delay(150);
    writeCommand(0x01); delay(150);
    writeCommand(0x11); lastSleepOutAt = millis(); delay(120);
    writeCommand(0x3A); writeData(0x55);
    writeCommand(0x36); writeData(0xC8);
    writeCommand(0x13); delay(10);
    writeCommand(0x21); delay(10);
    writeCommand(0x29); delay(100);
    lcdSPI.endTransaction();
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });
    display = lv_display_create(SCREEN_W, SCREEN_H);
    if (!display) { heap_caps_free(buffer); return false; }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, nullptr, 240 * 32 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    input = lv_indev_create();
    if (!input) { lv_display_delete(display); display = nullptr; heap_caps_free(buffer); return false; }
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, readInput);
    lv_timer_set_period(lv_indev_get_read_timer(input), 5);
    Power::uiActivity();
    createScreens();
    ready = true;
    flushDisplay();
    setDisplayBrightness(70);
    Serial.println("Display: LVGL 9, ST7789 RGB565, 240x32 partial buffer");
    return true;
}
void setDisplayBrightness(uint8_t percent) {
    static bool pwmReady = false;
    if (!pwmReady) { ledcSetup(0, 5000, 8); ledcAttachPin(LCD_BL, 0); pwmReady = true; }
    ledcWrite(0, map(constrain(percent, 0, 100), 0, 100, 0, 255));
}
void setDisplayTouch(bool pressed, uint16_t x, uint16_t y) {
    if (pressed != touchDown || (pressed && (x != touchX || y != touchY))) Power::uiActivity();
    touchDown = pressed; touchX = x; touchY = y;
}
void suppressDisplayTouch() {
    touchDown = false;
    if (ready) { lv_indev_reset(input, nullptr); lv_indev_wait_release(input); }
    actionHead = actionCount = 0;
}
void serviceDisplay() {
    if (!ready || panelSleeping || sleepRequested) return;
    if (feedbackUntil && int32_t(millis() - feedbackUntil) >= 0) {
        feedbackUntil = 0; lv_obj_add_flag(feedback, LV_OBJ_FLAG_HIDDEN);
    }
    const uint32_t started = micros();
    const uint64_t transferBefore = totalFlushUs;
    lv_timer_handler();
    const uint32_t elapsed = micros() - started;
    const uint32_t transferUs = totalFlushUs - transferBefore;
    maxServiceUs = max(maxServiceUs, elapsed);
    // SPI time is bus-limited; do not boost the CPU just for waiting on transfers.
    Power::observeRender(elapsed > transferUs ? elapsed - transferUs : 0);
}
void flushDisplay() { if (ready && !panelSleeping && !sleepRequested) lv_refr_now(display); }
bool takeDisplayAction(DisplayAction &action) {
    if (!actionCount) return false;
    action = actions[actionHead]; actionHead = (actionHead + 1) % 16; --actionCount;
    return true;
}
void updateWifiStatus(bool connected, int rssi) {
    // Background status must neither touch the sleeping UI nor request a CPU boost.
    if (!ready || panelSleeping || sleepRequested) return;
    int level = connected ? (displayedWifiLevel > 0 ? displayedWifiLevel : 1) : 0;
    // RSSI() returns zero on failure; retain the last level until a valid sample.
    if (connected && rssi < 0) {
        if (displayedWifiLevel <= 0) level = rssi >= -60 ? 3 : rssi >= -75 ? 2 : 1;
        else {
            // Nominal boundaries -75/-60 dBm, with 3 dB hysteresis each way.
            while (level < 3 && rssi >= (level == 1 ? -72 : -57)) ++level;
            while (level > 1 && rssi < (level == 3 ? -63 : -78)) --level;
        }
    }
    if (level == displayedWifiLevel) return;
    displayedWifiLevel = level;
    lv_obj_invalidate(wifi);
}
void updateDeviceSelector(bool pcSelected) {
    if (!ready) return;
    Power::uiActivity();
    lv_obj_set_style_bg_color(pc, lv_color_hex(pcSelected ? 0x27523C : SURFACE), 0);
    lv_obj_set_style_border_color(pc, lv_color_hex(pcSelected ? ACCENT : 0x354353), 0);
    lv_obj_set_style_bg_color(roku, lv_color_hex(pcSelected ? SURFACE : 0x27523C), 0);
    lv_obj_set_style_border_color(roku, lv_color_hex(pcSelected ? 0x354353 : ACCENT), 0);
}
void displayStatus(bool connected, const String&, const String&, bool pcSelected) {
    if (!ready) return;
    suppressDisplayTouch();
    updateWifiStatus(connected); updateDeviceSelector(pcSelected);
    if (lv_screen_active() != home) lv_screen_load(home);
    flushDisplay();
}
void displaySettings(uint8_t brightness, uint16_t sleep) {
    if (!ready) return;
    Power::uiActivity();
    suppressDisplayTouch();
    updateBrightnessSlider(brightness); updateSleepSlider(sleep);
    if (lv_screen_active() != settings) lv_screen_load(settings);
    flushDisplay();
}
void displayLights() {
    if (!ready) return;
    Power::uiActivity();
    suppressDisplayTouch();
    lv_slider_set_value(lightBrightnessSlider, 50, LV_ANIM_OFF);
    lv_slider_set_value(lightTemperatureSlider, 4000, LV_ANIM_OFF);
    lv_label_set_text(lightBrightnessValue, "50%");
    lv_label_set_text(lightTemperatureValue, "4000K");
    if (lv_screen_active() != lights) lv_screen_load(lights);
    flushDisplay();
}
bool isLightsScreenActive() {
    return ready && lv_screen_active() == lights;
}
void updateBrightnessSlider(uint8_t value) {
    if (!ready) return;
    Power::uiActivity();
    lv_slider_set_value(brightnessSlider, value, LV_ANIM_OFF);
    lv_label_set_text_fmt(brightnessValue, "%u%%", unsigned(value));
}
void updateSleepSlider(uint16_t value) {
    if (!ready) return;
    Power::uiActivity();
    lv_slider_set_value(sleepSlider, constrain(value, 2, 120), LV_ANIM_OFF);
    if (!value) lv_label_set_text(sleepValue, "Off");
    else lv_label_set_text_fmt(sleepValue, "%u sec", unsigned(value));
}
void updateBatteryStatus(uint8_t percent, float volts) {
    if (!ready) return;
    Power::uiActivity();
    percent = constrain(percent, 0, 100);
    const char* icon = percent > 80 ? LV_SYMBOL_BATTERY_FULL : percent > 60 ? LV_SYMBOL_BATTERY_3 :
                       percent > 40 ? LV_SYMBOL_BATTERY_2 : percent > 20 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char text[32];
    snprintf(text, sizeof(text), "%.2fV %s", double(volts), icon);
    lv_label_set_text(battery, text);
    lv_obj_set_style_text_color(battery, lv_color_hex(percent <= 20 ? 0xFF9393 : TEXT), 0);
    flushDisplay();
}
void displayUpdateStatus(const char* message) {
    if (!ready) return;
    Power::uiActivity();
    lv_label_set_text(updateMessage, message);
    // Maintenance calls this outside lv_timer_handler before synchronous work.
    flushDisplay();
}
void displayFeedback(const char* message) {
    if (!ready) return;
    Power::uiActivity();
    lv_label_set_text(feedback, message);
    lv_obj_remove_flag(feedback, LV_OBJ_FLAG_HIDDEN);
    feedbackUntil = millis() + 2500;
}
void printDisplayStats() {
    Serial.printf("Display: max service %lu us; max rectangle transfer %lu us; heap %u\n",
                  (unsigned long)maxServiceUs, (unsigned long)maxFlushUs, ESP.getFreeHeap());
}

void serviceDisplayPower() {
    if (!ready || !sleepRequested || panelSleeping) return;
    // ST7789 requires 120 ms between Sleep Out and the next Sleep In.
    if (millis() - lastSleepOutAt < 120) return;
    lcdSPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
    writeCommand(0x28); // display off
    writeCommand(0x10); // oscillator and panel supplies off; GRAM retained
    lcdSPI.endTransaction();
    panelSleeping = true;
    delay(5); // command settling, per ST7789T3 datasheet
}
void setDisplaySleeping(bool sleeping) {
    sleepRequested = sleeping;
    if (!ready) return;
    if (sleeping) { setDisplayBrightness(0); serviceDisplayPower(); return; }
    if (!panelSleeping) return;
    Power::uiActivity();
    lcdSPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
    writeCommand(0x11);
    lcdSPI.endTransaction();
    lastSleepOutAt = millis();
    delay(5); // only Sleep In requires the longer 120 ms exclusion window
    lcdSPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
    writeCommand(0x29);
    lcdSPI.endTransaction();
    panelSleeping = false;
}
bool isDisplaySleeping() { return !ready || panelSleeping; }
