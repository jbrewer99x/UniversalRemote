# Reliability audit — 2026-09-05

Scope: ESP32-S3 handheld firmware and FastAPI/browser server, prioritizing the existing GPIO18 active-low physical button. This is a source audit; hardware behavior and current draw have not been measured. Pin assignments, display orientation, touch driver, power latch, device command routing, and HTTP response contracts are preserved.

## First increment implemented

- **Sleep deadlock:** `main.cpp::enterLightSleep` previously started `/content/going-to-sleep.wav`, then waited on `isAudioPlaying()` without calling `serviceAudio()`. The bundled audio library sets its running flag when opening the file and needs `audio.loop()` to feed/process the file. With that sound available, the screen could go dark without ever reaching `esp_light_sleep_start()`.
- Sleep preparation now runs over successive loop iterations while audio is serviced. The button must be released stably for 40 ms before arming low-level GPIO wake. Stuck audio or a held/stuck button cancels preparation after 10 seconds and restores the display.
- Both GPIO wake setup results are checked. Failure leaves the screen usable. Wake cause, result, and pin level are logged. The wake press is consumed until a stable release, preventing immediate re-sleep.
- **Slow wake:** the previous code waited for button release and then for Wi-Fi association (up to 25 seconds) before restoring brightness. It now restores brightness immediately after sleep returns and starts Wi-Fi association in the background. Reconnect attempts also avoid the blocking association loop. The small Wi-Fi indicator updates when connectivity changes.
- The loop resamples input and time after sleep. Inactivity checks use a fresh clock reading so `noteActivity()` cannot make unsigned subtraction wrap when an earlier loop timestamp is used.
- Command polling runs after local input and is deferred while audio is playing. Automatic OTA/SD maintenance waits until the screen is off and audio/find/shutdown/sleep preparation are inactive. Downloads themselves remain synchronous.
- Pickup during Find Remote now stops the active sound, rather than only lowering its volume. Battery display and warning logic share one voltage sample instead of two potentially different readings.
- Firmware and SD manifest handlers in `server/app.py` are ordinary synchronous FastAPI handlers, so their disk reads/hashing execute in the worker pool instead of blocking the async event loop. Response fields are unchanged.

## LVGL and responsiveness increment implemented

The requested framework migration supersedes the earlier recommendation to retain the raw UI renderer. Home and Settings now use pinned LVGL 9.2.2 widgets at the existing positions, with a polished dark theme, a 240×32 RGB565 buffer, bulk SPI rectangle writes, pressed states, and draggable sliders. Changed settings save on release/navigation/sleep. Battery percentage and voltage remain visible; updating battery state on Settings no longer overwrites its header.

Normal command POSTs and Find Remote polls now run in one worker with an eight-command FIFO, captured device selection, one/two-second connect/read timeouts, three-second unsent expiry, no POST retries, and bounded poll backoff. Results return to the main loop. Sleep and the explicit manual-maintenance state pause the worker and wait cooperatively for active work; stale completions are discarded. Updates flush their busy message before synchronous work. The current firmware has manual updates only; the automatic-maintenance description above records the earlier increment.

Touch state now survives IRQ-high report gaps, filters empty reports for 25 ms, and suppresses wake/navigation gestures until release. Actual CST3530 release semantics and I2C error recovery remain hardware acceptance items.

See [firmware UI details and acceptance checks](firmware-ui.md). Host tests cover production queue/touch/settings policies and the real LVGL display implementation with a simulated SPI panel, including all control centers, holds, taps/drags, partial redraws, and render-buffer allocation failure.

## Remaining findings, in priority order

| Priority | Evidence and effect | Next bounded change |
| --- | --- | --- |
| P1 | IRQ-high gaps no longer create releases; SensorLib still represents both empty/release reports and failed reads as no points. | Validate stationary holds, release/re-tap, wake consumption, and I2C faults on the actual CST3530. The host tests cannot establish the electrical report semantics. |
| P1 | `server/app.py::get_remote_command` clears one process-global slot before the ESP32 confirms receipt. A lost response loses Find Remote; process restart loses it; multiple workers have different slots. `enterLightSleep` switches Wi-Fi off with only GPIO wake, so Find Remote cannot reach the device in true light sleep. | Add command IDs, expiry and acknowledgement with backward-compatible endpoints; use shared persistence if multiple workers are required. Separately choose whether timed wake/network availability is worth additional battery drain. |
| P1 | SD/OTA installation remains intentionally synchronous inside dedicated maintenance. Normal networking is quiesced and ordinary audio is stopped first, but the OTA starting-sound wait remains unbounded. SD replacement still deletes before rename. | Add download/sound deadlines and recoverable SD replacement separately, retaining the SHA256 checks. |
| P2 | `server/static/remote.js::api` and `sendCommand` have no request deadline. A hung status request keeps `statusRefreshInFlight` true, suppressing later refreshes even after device selection; Find Remote can remain disabled indefinitely. Rapid command fetches are independent and can finish out of order. | Add AbortController deadlines and selection-specific status cancellation. Serialize commands where order matters; show failure without replaying toggles. |
| P2 | `main.cpp::serviceFindRemote` retries a missing/failed WAV every loop. Find Remote and sound-test flags can both be active and compete for one audio player. Low-battery playback can be replaced by another sound, and its shutdown wait has no deadline. Initial battery checks happen before SD/audio initialization. | Introduce a small audio owner/priority policy, retry cooldown, and bounded critical-shutdown message. Initialize audio before checks that may play warnings. |
| P2 | Manifest hashes are recomputed on every request; the worker-pool fix prevents event-loop stalls but does not reduce disk work. Firmware binary/version/hash reads are separate, so concurrent publishing can create mismatched metadata. Aggregate device status is sequential. | Publish immutable artifacts with an atomic manifest; cache manifests against a generation. Add bounded per-device status concurrency separately. |
| P2 | `platformio.ini` leaves the platform and SensorLib unpinned; README still describes firmware 0.2.0 while source is 0.2.19. Sleep logic is duplicated in backup/patch scripts, and operational state is spread across globals. | Record the exact hardware-tested dependency versions, refresh the hardware/button documentation, and retire obsolete patch scripts after review. Extract power state only after this fix is validated. |

## Hardware acceptance checklist

1. With the normal SD card and sleep WAV present, press/release GPIO18. Expect preparation, sound completion, then `Sleep: entering light sleep; GPIO18=1`.
2. Press again. Expect screen restoration while still holding the button, and a log containing sleep result/wake cause. Release and repeat at least 20 cycles, on USB and battery power.
3. Disable the access point, then repeat sleep/wake. The display must return before Wi-Fi reconnects; touch/settings remain usable offline. Re-enable the AP and confirm the indicator and remote commands recover.
4. Hold the sleep button longer than 10 seconds. Preparation must cancel with a visible screen, with no repeated sleep until release and a fresh press. Repeat with the sleep WAV absent and with a deliberately invalid test WAV on a spare SD card.
5. Confirm GPIO7 keeps the battery power latch asserted during light sleep. Do not substitute GPIO6 (the board BAT POWER input) for the separately wired GPIO18 button.
6. Verify ordinary screen-off still wakes by touch/motion, its first touch does not issue a remote command, and the one-hour idle transition still reaches true light sleep. True light sleep currently wakes only from the wired button.
7. Test PC/Roku selection, commands, settings, audio test, and Find Remote/pickup. Exercise server/AP outages while tapping and dragging; ordinary HTTP now runs in the worker. Measure the 100 ms visual-feedback target on hardware.
8. Updates are manual in the current firmware. Check Settings and serial updates, including an ordinary HTTP request in flight. Confirm the busy message is drawn before installation and SD updates do not overlap ordinary playback or worker requests.

## Validation

The LVGL increment builds on Linux with Espressif32 7.0.1, Arduino ESP32 2.0.17, LVGL 9.2.2, ArduinoJson 7.4.3, SensorLib 0.4.1, and bundled ESP32-audioI2S 2.0.0. Firmware version stays at the existing 0.2.24. Final size and test results are recorded in [firmware-ui.md](firmware-ui.md). The C++17 inline-variable warnings in the existing config remain. No hardware was flashed or server redeployed.

`python3 firmware/esp32_remote/tests/run_host_tests.py --build-dir /tmp/remote-lvgl-host` passes policy, real LVGL interaction/redraw, and forced render-buffer allocation-failure checks. Rendered Home/Settings captures were inspected for text fit and layout; these do not measure physical latency or SPI correctness on the board.

The following results describe the earlier audit increment:

Server tests: `python -m unittest discover -s server/tests -v`, using FastAPI 0.116.1 and HTTPX 0.28.1 as pinned by the repo. Four tests cover manifest fields/hashes, deterministic SD hashes and change detection, unavailable-storage protection, and command delivery while each manifest is deliberately blocked in hashing. The registry is replaced with an empty test registry; no device configuration is read and no hardware requests are sent.

Firmware build command: `platformio run --project-dir firmware/esp32_remote --environment waveshare_esp32_s3_touch_lcd_2_8`. Build passed using a temporary drive mapping (Windows compiler subprocesses reject a UNC working directory). RAM: 51,908 / 327,680 bytes (15.8%); application flash: 1,339,333 / 6,291,456 bytes (21.3%). Resolved dependencies: Espressif32 7.1.1, Arduino ESP32 2.0.17, ArduinoJson 7.4.3, SensorLib 0.4.1, bundled ESP32-audioI2S 2.0.0. Existing config.h inline-variable/C++17 warnings remain; server tests report existing on_event deprecation warnings. Firmware version remains 0.2.19; version bump and publishing belong to a subsequent release step. No firmware has been flashed or published and the live server has not been redeployed. A successful build does not validate the electrical wake path or battery power retention.

References: [Espressif ESP32-S3 light-sleep GPIO wake documentation](https://docs.espressif.com/projects/esp-idf/en/v4.4.4/esp32s3/api-reference/system/sleep_modes.html) and [FastAPI synchronous handler thread-pool behavior](https://fastapi.tiangolo.com/async/#path-operation-functions).
