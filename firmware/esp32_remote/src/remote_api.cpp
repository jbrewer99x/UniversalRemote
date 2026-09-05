#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "remote_api.h"
#include "secrets.h"

namespace {
RemoteNetwork::Scheduler scheduler;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t results = nullptr;
TaskHandle_t worker = nullptr;
uint32_t generation = 0;

bool request(bool poll, const RemoteNetwork::Command &command, RemoteResult &result) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.setConnectTimeout(1000);
    http.setTimeout(2000);
    http.setReuse(false);
    if (!http.begin(String(REMOTE_SERVER_URL) +
                    (poll ? "/api/remote/command" : "/api/command"))) return false;
    int status;
    if (poll) status = http.GET();
    else {
        http.addHeader("Content-Type", "application/json");
        JsonDocument doc;
        doc["device"] = command.device;
        doc["command"] = command.command;
        String body;
        serializeJson(doc, body);
        status = http.POST(body);
    }
    bool ok = status >= 200 && status < 300;
    if (ok && poll) {
        // This endpoint returns a small JSON object with Content-Length.
        // Bound both allocation and total body-read time.
        char body[256];
        size_t length = 0;
        const uint32_t started = millis();
        auto *stream = http.getStreamPtr();
        const int expected = http.getSize();
        ok = expected > 0 && expected < static_cast<int>(sizeof(body));
        while (ok && length < static_cast<size_t>(expected) && millis() - started < 2000) {
            if (stream->available()) body[length++] = static_cast<char>(stream->read());
            else if (!http.connected()) break;
            else vTaskDelay(pdMS_TO_TICKS(1));
        }
        body[length] = '\0';
        JsonDocument doc;
        ok = ok && length == static_cast<size_t>(expected) && !deserializeJson(doc, body);
        if (ok) {
            const char *value = doc["command"] | "";
            ok = strlen(value) < sizeof(result.command);
            if (ok) strcpy(result.command, value);
        }
    }
    http.end();
    return ok;
}
void networkTask(void*) {
    for (;;) {
        // Reserve completion capacity before starting; never lose a POST result.
        if (!uxQueueSpacesAvailable(results)) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); continue; }
        RemoteNetwork::Command command;
        const bool online = WiFi.status() == WL_CONNECTED;
        portENTER_CRITICAL(&stateLock);
        const auto work = scheduler.begin(millis(), online, command);
        const uint32_t workGeneration = generation;
        const uint32_t waitMs = scheduler.waitMs(millis(), online);
        portEXIT_CRITICAL(&stateLock);
        if (work == RemoteNetwork::Work::None) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(max(uint32_t(1), waitMs)));
            continue;
        }
        RemoteResult result;
        result.generation = workGeneration;
        result.poll = work == RemoteNetwork::Work::Poll;
        result.expired = work == RemoteNetwork::Work::Expired;
        result.success = !result.expired && request(result.poll, command, result);
        if (!result.poll) strcpy(result.command, command.command);
        // Publish before marking idle; pause + idle guarantees no later completion.
        portENTER_CRITICAL(&stateLock);
        const bool discard = scheduler.isPaused() || generation != workGeneration;
        portEXIT_CRITICAL(&stateLock);
        if (!discard) xQueueSend(results, &result, 0);
        portENTER_CRITICAL(&stateLock);
        scheduler.finish(work, result.success, millis());
        portEXIT_CRITICAL(&stateLock);
    }
}
}
bool initRemoteNetwork() {
    if (worker) return true;
    results = xQueueCreate(16, sizeof(RemoteResult));
    if (!results) return false;
    if (xTaskCreate(networkTask, "remote-http", 8192, nullptr, 1, &worker) != pdPASS) {
        vQueueDelete(results);
        results = nullptr;
        return false;
    }
    return true;
}
RemoteNetwork::Submit queueRemoteCommand(const char* device, const char* command) {
    if (!worker) return RemoteNetwork::Submit::Paused;
    const bool online = WiFi.status() == WL_CONNECTED;
    portENTER_CRITICAL(&stateLock);
    const auto result = scheduler.submit(device, command, millis(), online);
    portEXIT_CRITICAL(&stateLock);
    if (result == RemoteNetwork::Submit::Accepted) xTaskNotifyGive(worker);
    return result;
}
bool takeRemoteResult(RemoteResult &result) {
    while (results && xQueueReceive(results, &result, 0) == pdTRUE) {
        if (worker) xTaskNotifyGive(worker);
        portENTER_CRITICAL(&stateLock);
        const bool current = result.generation == generation && !scheduler.isPaused();
        portEXIT_CRITICAL(&stateLock);
        if (current) return true;
    }
    return false;
}
void setRemotePolling(bool enabled) {
    portENTER_CRITICAL(&stateLock);
    const bool changed = scheduler.allowPolling(enabled);
    portEXIT_CRITICAL(&stateLock);
    if (changed && worker) xTaskNotifyGive(worker);
}
void setRemoteScreenOff(bool off) {
    portENTER_CRITICAL(&stateLock);
    const bool changed = scheduler.setScreenOff(off, millis());
    portEXIT_CRITICAL(&stateLock);
    if (changed && worker) xTaskNotifyGive(worker);
}
void pauseRemoteNetwork() {
    portENTER_CRITICAL(&stateLock);
    if (!scheduler.isPaused()) ++generation;
    scheduler.pause();
    portEXIT_CRITICAL(&stateLock);
    if (worker) xTaskNotifyGive(worker);
}
bool remoteNetworkIdle() {
    portENTER_CRITICAL(&stateLock);
    const bool idle = scheduler.idle();
    portEXIT_CRITICAL(&stateLock);
    return idle;
}
void resumeRemoteNetwork() {
    RemoteResult ignored;
    while (takeRemoteResult(ignored)) {}
    portENTER_CRITICAL(&stateLock);
    scheduler.resume(millis());
    portEXIT_CRITICAL(&stateLock);
    if (worker) xTaskNotifyGive(worker);
}
