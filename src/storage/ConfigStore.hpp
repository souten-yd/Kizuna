#pragma once

#include <Arduino.h>

#include "AppConfig.hpp"
#include "Board.hpp"

struct DeviceConfig {
    String wifiSsid;
    String wifiPassword;
    String serverHost;
    uint16_t serverPort = appcfg::kDefaultServerPort;
    String serverPath = appcfg::kDefaultServerPath;
    String deviceName = board::kDefaultDeviceName;
    String packName = "kizuna";
    uint8_t volume = appcfg::kSpeakerVolume;
    uint8_t brightness = 160;
    uint8_t ledBrightness = appcfg::kLedBrightness;
    bool swayEnabled = true;
    // Give up range, the LED bar and some backlight while on battery. On by
    // default: the failure it avoids is the device locking up mid-sentence,
    // and the cost is a bar of LEDs nobody is looking at.
    bool batterySaver = true;
    int8_t txPowerDbm = appcfg::kBatteryTxPowerDbm;
    // The device's own web page: status, log, boot history, OTA, packs. It is
    // a second TCP server on a part whose whole heap is smaller than a JPEG,
    // which is why it used to be opt-in - but it is also the only way to see
    // inside a device with no cable attached, and the bugs worth chasing are
    // the ones that only happen with no cable attached. So it is on by
    // default now, and the flag is there to turn it off rather than on.
    bool webServerEnabled = true;
    // Guards the OTA port and the browser's install button alike. Empty means
    // no password, which on a home LAN is a choice rather than an accident.
    String otaPassword;

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
