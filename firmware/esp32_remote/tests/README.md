# Firmware host checks

From the repository root, after PlatformIO has resolved LVGL 9.2.2:

```sh
python3 firmware/esp32_remote/tests/run_host_tests.py --build-dir /tmp/remote-lvgl-host
```

Requires Python 3 and `gcc`/`g++`. The runner compiles production scheduler/touch/settings policy code and the production display implementation against the real pinned LVGL sources. Small Arduino/SPI adapters replace hardware calls; SPI rectangle writes are captured into a 240×320 framebuffer. Build files and PNG/PPM captures stay in the supplied temporary directory.

Checks cover FIFO order and captured device, full/offline/paused rejection, expiry including clock rollover, no POST retry, one active operation, poll priority/backoff, pause and in-flight completion, retained touch state through interrupt gaps, release filtering, wake-touch consumption, and dirty settings after failed saves. Display tests exercise all control centers, held presses, taps and drags at slider endpoints, release save events, screen changes, maintenance suppression, idle/partial redraws, and render-buffer allocation failure.

The fake clock and SPI bus do not measure ESP32 latency. Actual touch-controller release reports, slow/unavailable HTTP servers, audio, sleep/current draw, and OTA/SD installation still require the device. See [the UI validation checklist](../../../docs/firmware-ui.md).
