# Universal Remote ESP32 bootstrap 0.1.0

Target: Waveshare ESP32-S3-Touch-LCD-2.8 (SKU 27690).

This first firmware intentionally focuses on:
- USB serial bring-up
- 16 MB flash / 8 MB PSRAM configuration
- Wi-Fi
- NVS preferences
- automatic 60-second TrueNAS update checks
- manual update check from serial
- streamed OTA into the inactive partition
- SHA-256 verification before activating the new firmware

Display/touch are intentionally deferred until the board arrives because
Waveshare changed the touch controller: V1=CST328, V2=CST3530.

## Before building
Copy:
    include/secrets.example.h
to:
    include/secrets.h

Set:
    WIFI_SSID
    WIFI_PASSWORD
    REMOTE_SERVER_URL

`secrets.h` is ignored by Git.

## PlatformIO
Open `firmware/esp32_remote` in VS Code with PlatformIO.

Then:
    Build
    Upload
    Monitor

Serial monitor: 115200 baud.

Commands:
    h  help
    i  board/network info
    c  check for update without installing
    u  check and install
    a  toggle 60-second auto update
    r  reboot

## Firmware version
Change `FIRMWARE_VERSION` in `include/config.h` for every published release.

The build output will be under:
    .pio/build/waveshare_esp32_s3_touch_lcd_2_8/firmware.bin

Publish that file to the TrueNAS OTA repository using the server-side
publish script.
