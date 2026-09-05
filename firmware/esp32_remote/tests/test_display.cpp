#include <cassert>
#include <cstring>
#include <fstream>
#include <lvgl.h>
#include "display.h"
#include "SPI.h"
#include "esp_heap_caps.h"

void step(bool down, int x, int y, unsigned duration = 30) {
    setDisplayTouch(down, x, y);
    for (unsigned t = 0; t < duration; t += 5) { delay(5); serviceDisplay(); }
}
void expectAction(const char* name, int value = -1) {
    DisplayAction action;
    if (!takeDisplayAction(action)) { fprintf(stderr, "Missing action: %s\n", name); abort(); }
    assert(!strcmp(action.name, name));
    if (value >= 0) assert(action.value == value);
}
void expectEmpty() { DisplayAction action; assert(!takeDisplayAction(action)); }
void snapshot(const char* name) {
    flushDisplay();
    std::ofstream out(name, std::ios::binary);
    out << "P6\n240 320\n255\n";
    for (uint16_t pixel : testPixels) {
        const char rgb[] = {char(((pixel >> 11) & 31) * 255 / 31),
                            char(((pixel >> 5) & 63) * 255 / 63), char((pixel & 31) * 255 / 31)};
        out.write(rgb, 3);
    }
}

int main(int argc, char**) {
    if (argc > 1) {
        testAllocationFailure = true;
        assert(!initDisplay());
        displaySettings(75, 30); displayStatus(true, "", "", true);
        updateBatteryStatus(80, 4.0f); displayFeedback("error");
        displayUpdateStatus("error"); serviceDisplay(); flushDisplay();
        expectEmpty();
        puts("PASS: render-buffer allocation failure is safe");
        return 0;
    }
    assert(initDisplay());
    displayStatus(true, "", "", true);
    updateBatteryStatus(80, 4.0f);
    step(false, 0, 0);
    auto* statusBar = lv_obj_get_child(lv_screen_active(), 0);
    const char* batteryText = lv_label_get_text(lv_obj_get_child(statusBar, 2));
    assert(strstr(batteryText, "4.00V") && !strchr(batteryText, '%'));
    snapshot("home.ppm");
    // Header/settings, selector, and every existing remote command hit area.
    struct Control { int x, y; const char* name; };
    const Control controls[] = {{120,14,"settings"}, {62,51,"pc"}, {178,51,"roku"},
        {43,90,"power"}, {120,90,"home"}, {197,90,"back"}, {120,130,"up"},
        {49,170,"left"}, {120,170,"ok"}, {191,170,"right"}, {49,210,"previous"},
        {120,210,"down"}, {191,210,"next"}, {43,250,"rewind"}, {120,250,"play_pause"},
        {197,250,"fast_forward"}, {43,292,"volume_down"}, {120,292,"mute"}, {197,292,"volume_up"}};
    for (const auto &control : controls) {
        step(true, control.x, control.y);
        expectAction(control.name);
        step(true, control.x, control.y, 1200); // holding never repeats a command
        expectEmpty();
        step(false, control.x, control.y);
        expectEmpty();
    }
    displayLights();
    assert(isLightsScreenActive());
    step(false, 0, 0);
    const Control lightsControls[] = {{60, 64, "govee_on"}, {180, 64, "govee_off"},
        {50, 231, "govee_warm"}, {122, 231, "govee_cool"}, {192, 231, "govee_red"},
        {50, 271, "govee_green"}, {122, 271, "govee_blue"}, {192, 271, "govee_party"},
        {120, 301, "govee_crazy_toggle"}};
    for (const auto &control : lightsControls) {
        step(true, control.x, control.y);
        expectAction(control.name);
        step(false, control.x, control.y);
        expectEmpty();
    }
    step(true, 30, 18);
    expectAction("show_home");
    step(false, 30, 18);
    expectEmpty();
    displayStatus(true, "", "", true);
    const size_t before = testTransferredPixels;
    step(false, 0, 0, 100);
    assert(testTransferredPixels == before); // idle screen does not redraw
    updateWifiStatus(false);
    step(false, 0, 0);
    assert(testTransferredPixels - before < 240 * 32); // status update stays local

    displaySettings(75, 30);
    step(false, 0, 0);
    snapshot("settings.ppm");
    step(true, 173, 86);
    expectEmpty();
    step(true, 14, 86);
    expectAction("brightness", 0);
    step(true, 226, 86);
    expectAction("brightness", 100);
    step(false, 226, 86);
    expectAction("save_settings"); expectEmpty();
    step(true, 14, 154); expectEmpty();
    step(false, 14, 154); expectAction("sleep", 2);
    expectAction("save_settings");
    step(true, 14, 154); expectEmpty();
    step(true, 226, 154); expectAction("sleep", 120);
    step(false, 226, 154); expectAction("save_settings"); expectEmpty();
    step(true, 120, 235); expectAction("updates");
    suppressDisplayTouch();
    step(true, 120, 235, 500); expectEmpty(); // maintenance ignores the held touch
    step(false, 120, 235);
    step(true, 120, 285); expectAction("sounds");
    step(false, 120, 285);
    step(true, 30, 18); expectAction("show_home");
    displayStatus(false, "", "", false);
    step(true, 30, 18, 500); expectEmpty(); // navigation cannot activate the next screen
    step(false, 30, 18);
    displaySettings(0, 0);
    step(false, 0, 0);
    snapshot("settings-off.ppm");
    setDisplaySleeping(true);
    assert(isDisplaySleeping());
    assert(testLcdCommands.back().first == 0x10);
    const auto sleepingPixels = testTransferredPixels;
    updateBatteryStatus(10, 3.2f);
    step(false, 0, 0, 500);
    flushDisplay();
    assert(testTransferredPixels == sleepingPixels);
    setDisplaySleeping(false);
    assert(!isDisplaySleeping());
    assert(testLcdCommands[testLcdCommands.size() - 2].first == 0x11);
    assert(testLcdCommands.back().first == 0x29);
    assert(testLcdCommands.back().second - testLcdCommands[testLcdCommands.size() - 2].second >= 5);
    const auto afterWakeCommands = testLcdCommands.size();
    setDisplaySleeping(true);
    assert(!isDisplaySleeping()); // respect Sleep Out -> Sleep In exclusion
    assert(testLcdCommands.size() == afterWakeCommands);
    delay(120); serviceDisplayPower();
    assert(isDisplaySleeping());
    setDisplaySleeping(false);
    step(false, 0, 0);
    puts("PASS: real LVGL layout, hit areas, holds, sliders, navigation, partial redraws");
}
