#pragma once

#include <Arduino.h>

#include "AppConfig.hpp"

struct DeviceConfig {
    String wifiSsid;
    String wifiPassword;

    // Legacy/current companion bridge. This remains the default so existing
    // installations keep using the WebSocket server unchanged.
    String serverHost;
    uint16_t serverPort = appcfg::kDefaultServerPort;
    String serverPath = appcfg::kDefaultServerPath;

    // Voice transport selector:
    //   bridge      -> existing WebSocket companion_server
    //   qnap_stream -> direct QnapAssistant HTTP chunked upload + multipart audio
    String voiceTransport = "bridge";
    String qnapHost;
    uint16_t qnapPort = 11435;
    String qnapPath = "/v1/voice/chat/stream?profile=m5go";

    String deviceName = "M5GO-Companion";
    String packName = "kizuna";
    uint8_t volume = appcfg::kSpeakerVolume;
    uint8_t brightness = 160;
    uint8_t ledBrightness = appcfg::kLedBrightness;
    bool swayEnabled = true;
    // The pack manager web page. Useful, but it is a second TCP server on
    // a part whose whole heap is smaller than a JPEG, so it is opt-in.
    bool packServerEnabled = false;

    bool hasWifi() const { return !wifiSsid.isEmpty(); }
    bool usesQnapStream() const { return voiceTransport.equalsIgnoreCase("qnap_stream"); }
    bool hasServer() const {
        return usesQnapStream() ? !qnapHost.isEmpty() : !serverHost.isEmpty();
    }
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
