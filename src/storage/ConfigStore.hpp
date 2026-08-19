#pragma once

#include <Arduino.h>

#include "AppConfig.hpp"

struct DeviceConfig {
    String wifiSsid;
    String wifiPassword;
    String serverHost;
    uint16_t serverPort = appcfg::kDefaultServerPort;
    String serverPath = appcfg::kDefaultServerPath;
    String deviceName = "M5GO-Companion";
    String packName = "kizuna";
    uint8_t volume = appcfg::kSpeakerVolume;
    uint8_t brightness = 160;
    uint8_t ledBrightness = appcfg::kLedBrightness;
    bool swayEnabled = true;

    bool hasWifi() const { return !wifiSsid.isEmpty(); }
    bool hasServer() const { return !serverHost.isEmpty(); }
};

class ConfigStore {
public:
    bool begin();
    DeviceConfig load();
    bool save(const DeviceConfig& cfg);
    void clear();
    bool hasWifi();

    // Applies /companion/config/device.json from the card, if present, and
    // persists it to NVS. Editing a text file on the SD card beats typing a
    // password into a captive portal on a phone, and it is the only sane way
    // to provision a device that is sitting on a bench next to its own card
    // reader. Returns true when something was applied.
    bool mergeFromSd(DeviceConfig& cfg);

private:
    bool ready_ = false;
};
