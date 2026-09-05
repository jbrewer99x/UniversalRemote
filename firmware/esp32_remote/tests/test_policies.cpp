#include <assert.h>
#include <stdio.h>
#include "remote_policy.h"
#include "ui_policy.h"

using namespace RemoteNetwork;

void queueTests() {
    Scheduler scheduler;
    Command item;
    assert(scheduler.submit("pc", "power", 0, false) == Submit::Offline);
    assert(scheduler.submit("too-long-device", "power", 0, true) == Submit::Invalid);
    assert(scheduler.submit("pc", "power", 4999, true) == Submit::Accepted);
    assert(scheduler.submit("roku", "home", 4999, true) == Submit::Accepted);
    // Outgoing requests take priority even when a poll is due.
    assert(scheduler.begin(5000, true, item) == Work::Command);
    assert(!strcmp(item.device, "pc") && !strcmp(item.command, "power"));
    assert(scheduler.begin(5000, true, item) == Work::None); // one in flight
    scheduler.finish(Work::Command, false, 5001); // ambiguous failure: no retry
    assert(scheduler.begin(5001, true, item) == Work::Command);
    assert(!strcmp(item.device, "roku") && !strcmp(item.command, "home"));
    scheduler.finish(Work::Command, true, 5002);
    assert(scheduler.begin(5002, true, item) == Work::Poll);
    assert(scheduler.begin(5003, true, item) == Work::None);
    scheduler.finish(Work::Poll, true, 5004);
    assert(scheduler.begin(5005, true, item) == Work::None);

    for (int i = 0; i < 8; ++i) assert(scheduler.submit("pc", "up", 5005, true) == Submit::Accepted);
    assert(scheduler.submit("pc", "power", 5005, true) == Submit::Full);
    assert(scheduler.begin(8005, true, item) == Work::Expired);
    scheduler.finish(Work::Expired, false, 8005);
    scheduler.pause(); // discard queued requests, but never interrupt an active request
    assert(scheduler.idle());
    assert(scheduler.submit("pc", "power", 8005, true) == Submit::Paused);
    assert(scheduler.begin(9000, true, item) == Work::None);
    scheduler.resume(9000);
    assert(scheduler.begin(9000, true, item) == Work::None);
    scheduler.submit("pc", "power", 9000, true);
    assert(scheduler.begin(9000, true, item) == Work::Command);
    scheduler.pause();
    assert(!scheduler.idle());
    scheduler.finish(Work::Command, true, 9001);
    assert(scheduler.idle());

    // Expiry uses unsigned time differences across millis() rollover.
    Scheduler rollover;
    rollover.submit("pc", "mute", UINT32_MAX - 1000, true);
    assert(rollover.begin(1999, true, item) == Work::Expired);
}

void pollingTests() {
    Scheduler scheduler;
    Command item;
    uint32_t now = 5000;
    const unsigned delays[] = {1000, 2000, 4000, 8000, 10000, 10000};
    assert(scheduler.begin(now, false, item) == Work::None);
    for (unsigned delay : delays) {
        assert(scheduler.begin(now, true, item) == Work::Poll);
        scheduler.finish(Work::Poll, false, now);
        assert(scheduler.begin(now + delay - 1, true, item) == Work::None);
        now += delay;
    }
    assert(scheduler.begin(now, true, item) == Work::Poll);
    scheduler.finish(Work::Poll, true, now);
    assert(scheduler.begin(now + 4999, true, item) == Work::None);
    scheduler.allowPolling(false);
    assert(scheduler.begin(now + 5000, true, item) == Work::None);
    scheduler.allowPolling(true);
    assert(scheduler.begin(now + 5000, true, item) == Work::Poll);
}

void touchTests() {
    TouchState touch;
    using Sample = TouchState::Sample;
    assert(!touch.update(Sample::None, 0));
    assert(touch.update(Sample::Down, 10));
    assert(touch.update(Sample::None, 10000)); // stationary hold, IRQ gaps
    assert(touch.update(Sample::Up, 10001));
    assert(touch.update(Sample::Down, 10010)); // transient empty/error report
    assert(touch.update(Sample::None, 10040));
    assert(touch.update(Sample::Up, 10050));
    assert(touch.update(Sample::None, 10074));
    assert(!touch.update(Sample::None, 10075));
    assert(touch.update(Sample::Down, UINT32_MAX - 20));
    assert(touch.update(Sample::Up, UINT32_MAX - 10));
    assert(!touch.update(Sample::None, 15));
    WakeTouchGate gate;
    gate.suppress();
    assert(!gate.accept(true));
    assert(!gate.accept(true));
    assert(!gate.accept(false));
    assert(gate.accept(true));
}

void screenOffPollingTests() {
    Scheduler scheduler;
    Command item;
    scheduler.setScreenOff(true, 100);
    assert(scheduler.waitMs(100, true) == RemoteConfig::SCREEN_OFF_POLL_INTERVAL_MS);
    assert(scheduler.begin(100 + SCREEN_OFF_POLL_MS - 1, true, item) == Work::None);
    assert(scheduler.begin(100 + SCREEN_OFF_POLL_MS, true, item) == Work::Poll);
    scheduler.finish(Work::Poll, false, 100 + SCREEN_OFF_POLL_MS);
    assert(scheduler.waitMs(100 + SCREEN_OFF_POLL_MS, true) == SCREEN_OFF_POLL_MS);
    // Error backoff must not restore fast polling in screen-off mode.
    assert(scheduler.begin(1100 + SCREEN_OFF_POLL_MS, true, item) == Work::None);
    scheduler.setScreenOff(false, 1100 + SCREEN_OFF_POLL_MS);
    assert(scheduler.begin(1100 + SCREEN_OFF_POLL_MS, true, item) == Work::Poll);
    scheduler.finish(Work::Poll, true, 1100 + SCREEN_OFF_POLL_MS);
    assert(scheduler.waitMs(1100 + SCREEN_OFF_POLL_MS, true) == AWAKE_POLL_MS);
    scheduler.setScreenOff(true, UINT32_MAX - 100);
    assert(scheduler.begin(SCREEN_OFF_POLL_MS - 101, true, item) == Work::Poll);
    scheduler.finish(Work::Poll, true, SCREEN_OFF_POLL_MS - 101);
    scheduler.pause();
    assert(scheduler.waitMs(0, true) > 1000);
}

void settingsTests() {
    SavedSetting<uint8_t> setting;
    setting.saved = 75;
    assert(!setting.needsSave(75));
    assert(setting.needsSave(10));
    assert(setting.needsSave(20)); // dragging hasn't persisted intermediate values
    setting.committed(20, false);
    assert(setting.needsSave(20)); // failed NVS write must remain dirty
    setting.committed(20, true);
    assert(!setting.needsSave(20)); // release + navigation doesn't write twice
    assert(setting.needsSave(0));
    SavedSetting<uint16_t> sleep;
    sleep.saved = 0;
    assert(!sleep.needsSave(0)); // preserve stored Off until the user adjusts it
}

int main() {
    queueTests(); pollingTests(); screenOffPollingTests(); touchTests(); settingsTests();
    puts("PASS: queue ordering, expiry, rejection, no retries, quiescence, poll backoff, touch, settings");
}
