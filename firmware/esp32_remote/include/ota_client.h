#pragma once
#include <Arduino.h>
namespace OtaClient {
enum class Result { UpToDate, UpdateAvailable, Installed, NoWifi, ManifestError, InstallError };
struct Status {
    Result result = Result::ManifestError;
    String message;
    String currentVersion;
    String latestVersion;
};
void begin();
Status check(bool installIfAvailable = true);
}
