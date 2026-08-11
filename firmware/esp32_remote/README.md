# Universal Remote ESP32 Firmware 0.2.0

Target: Waveshare ESP32-S3-Touch-LCD-2.8 V2 (SKU 27690).

Version 0.2.0 represents the first functional handheld Universal Remote firmware. The ESP32-S3 now provides a standalone touchscreen remote interface backed by the Universal Remote server API.

## Current functionality

- Waveshare ESP32-S3-Touch-LCD-2.8 V2 hardware support
- 16 MB flash / 8 MB PSRAM configuration
- Battery-powered operation with firmware-controlled power latch
- ST7789 240x320 portrait display
- Correct display orientation and RGB color handling
- CST3530 capacitive touchscreen support
- Touch debouncing for UI navigation
- Wi-Fi connectivity with automatic reconnect
- Battery voltage monitoring and percentage display
- Persistent NVS preferences
- Adjustable display brightness
- Configurable sleep timer
- PC / Roku device selection
- Touchscreen remote-control interface
- Power, Home, and Back controls
- D-pad navigation with OK/Select
- Previous, Play/Pause, and Next media controls
- Volume Down, Mute, and Volume Up controls
- Remote commands sent through the Universal Remote server API
- Settings screen with device/network information
- Automatic OTA update checks
- Manual OTA update checks from the serial console
- Streamed OTA installation into the inactive partition
- SHA-256 firmware verification before activating an update

## Hardware

The firmware currently targets:

**Waveshare ESP32-S3-Touch-LCD-2.8 V2 (SKU 27690)**

Important V2 hardware details:

- ESP32-S3
- 16 MB flash
- 8 MB PSRAM
- 240x320 ST7789 LCD
- CST3530 capacitive touch controller
- Battery voltage ADC on GPIO 8
- Battery power control on GPIO 7
- Battery power button input on GPIO 6

The firmware asserts the battery power-control line during startup, allowing
the board to remain powered after the BAT POWER button is released.

## User interface

The main screen is optimized for handheld remote operation.

It currently provides:

- Wi-Fi status
- Battery percentage
- PC / Roku selector
- Power
- Home
- Back
- Up / Down / Left / Right
- OK
- Previous
- Play/Pause
- Next
- Volume Down
- Mute
- Volume Up

The selected device determines the target used for remote API commands.

The Settings screen provides configuration and status information including:

- Brightness
- Sleep timer
- Network information
- Battery percentage

Brightness and sleep settings are stored in NVS and restored after reboot.

## Before building

Copy:

    include/secrets.example.h

to:

    include/secrets.h

Set:

    WIFI_SSID
    WIFI_PASSWORD
    REMOTE_SERVER_URL

`secrets.h` is ignored by Git and should never be committed.

## PlatformIO

Open:

    firmware/esp32_remote

in VS Code with PlatformIO.

The PlatformIO environment is:

    waveshare_esp32_s3_touch_lcd_2_8

Build, upload, and monitor using the PlatformIO controls, or from a terminal.

### Build

    platformio run --environment waveshare_esp32_s3_touch_lcd_2_8

### Upload

    platformio run --environment waveshare_esp32_s3_touch_lcd_2_8 --target upload

### Serial monitor

    platformio device monitor --baud 115200

Serial monitor speed: **115200 baud**

## Serial console

Available commands:

    h  help
    i  board/network/battery information
    c  check for firmware update without installing
    u  check for and install firmware update
    a  toggle automatic OTA updates
    r  reboot

## OTA updates

The firmware can update itself over Wi-Fi from the Universal Remote server
running on TrueNAS.

The OTA process:

1. Requests the firmware manifest from the server.
2. Compares the published version with the installed firmware version.
3. Downloads the new firmware when an update is available.
4. Streams the image into the inactive OTA partition.
5. Calculates and verifies its SHA-256 hash.
6. Activates the new partition only after successful verification.
7. Reboots into the updated firmware.

Automatic update checks currently use the interval configured in
`include/config.h`.

## Firmware version

Change:

    FIRMWARE_VERSION

in:

    include/config.h

for every published firmware release.

The PlatformIO build output is:

    .pio/build/waveshare_esp32_s3_touch_lcd_2_8/firmware.bin

Publish that file to the TrueNAS OTA repository using the server-side
firmware publishing process.

## Current status

Version **0.2.0** is the first practical handheld release.

Core hardware bring-up is complete:

- Display works
- Touch works
- Battery operation works
- Wi-Fi works
- OTA works
- Persistent settings work
- Battery monitoring works
- Remote UI works
- PC/Roku selection works
- Remote API commands are integrated

Further development can now focus primarily on remote behavior, power
management, UI refinement, and additional functionality rather than basic
board bring-up.
