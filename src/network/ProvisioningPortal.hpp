#pragma once

#include <Arduino.h>

#include "storage/ConfigStore.hpp"

// First-run setup over a temporary access point.
//
// Blocking by design: there is nothing else worth doing until the device knows
// which network and which server it belongs to.
class ProvisioningPortal {
public:
    // Returns true when a configuration was saved (caller should reboot).
    bool run(ConfigStore& store, const DeviceConfig& current);

private:
    void drawScreen(const char* ssid, const char* ip, const char* status);
};
