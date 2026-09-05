#pragma once
#include "esp32-hal-bt.h"
constexpr int ESP_BT_CONTROLLER_STATUS_IDLE = 0;
inline int esp_bt_controller_get_status() { return testBluetoothOn ? 1 : ESP_BT_CONTROLLER_STATUS_IDLE; }
