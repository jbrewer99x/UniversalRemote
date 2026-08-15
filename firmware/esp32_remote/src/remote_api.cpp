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

bool checkRemoteCommand(String &command) {
    command = "";

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Remote API poll: skipped, Wi-Fi disconnected");
        return false;
    }

    HTTPClient http;

    String url =
        String(REMOTE_SERVER_URL) +
        "/api/remote/command";


    if (!http.begin(url)) {
        Serial.println("Remote API poll: HTTP begin failed");
        return false;
    }

    int status = http.GET();


    if (status < 200 || status >= 300) {
        http.end();
        return false;
    }

    String body = http.getString();


    http.end();

    JsonDocument doc;

    DeserializationError error =
        deserializeJson(doc, body);

    if (error) {
        Serial.printf(
            "Remote API poll: invalid JSON: %s\n",
            error.c_str()
        );
        return false;
    }

    const char *value = doc["command"];

    if (value != nullptr) {
        command = value;

        Serial.printf(
            "Remote API poll: command = %s\n",
            command.c_str()
        );
    } 

    return true;
}