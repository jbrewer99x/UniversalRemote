#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "remote_api.h"
#include "secrets.h"

bool sendRemoteCommand(
    const String &device,
    const String &command
) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Remote API: Wi-Fi disconnected");
        return false;
    }

    HTTPClient http;

    String url =
        String(REMOTE_SERVER_URL) +
        "/api/command";

    if (!http.begin(url)) {
        Serial.println("Remote API: HTTP begin failed");
        return false;
    }

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    JsonDocument doc;

    doc["device"] = device;
    doc["command"] = command;

    String body;
    serializeJson(doc, body);

    Serial.printf(
        "Remote API: %s -> %s\n",
        device.c_str(),
        command.c_str()
    );

    int status = http.POST(body);

    bool ok =
        status >= 200 &&
        status < 300;

    if (!ok) {
        Serial.printf(
            "Remote API: HTTP %d\n",
            status
        );
    }

    http.end();

    return ok;
}