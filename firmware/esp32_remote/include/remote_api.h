#pragma once
#include "remote_policy.h"
struct RemoteResult {
    uint32_t generation = 0;
    bool poll = false;
    bool success = false;
    bool expired = false;
    char command[32]{};
};
bool initRemoteNetwork();
RemoteNetwork::Submit queueRemoteCommand(const char* device, const char* command);
bool takeRemoteResult(RemoteResult &result);
void setRemotePolling(bool enabled);
void setRemoteScreenOff(bool off);
void pauseRemoteNetwork();
bool remoteNetworkIdle();
void resumeRemoteNetwork();
