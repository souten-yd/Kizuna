#include "ConfigStore.hpp"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>

namespace {
Preferences prefs;
}

bool ConfigStore::begin() {
    ready_ = prefs.begin(appcfg::kNvsNamespace, false);
    return ready_;
}

DeviceConfig ConfigStore::load() {
    DeviceConfig cfg;
    if (!ready_) return cfg;
    cfg.wifiSsid = prefs.getString("ssid", "");
    cfg.wifiPassword = prefs.getString("pass", "");
    cfg.serverHost = prefs.getString("host", "");
    cfg.serverPort = prefs.getUShort("port", appcfg::kDefaultServerPort);
    cfg.serverPath = prefs.getString("path", appcfg::kDefaultServerPath);
    cfg.deviceName = prefs.getString("name", board::kDefaultDeviceName);
    cfg.packName = prefs.getString("pack", "kizuna");
    cfg.volume = prefs.getUChar("vol", appcfg::kSpeakerVolume);
    cfg.brightness = prefs.getUChar("bright", 160);
    cfg.ledBrightness = prefs.getUChar("led", appcfg::kLedBrightness);
    cfg.swayEnabled = prefs.getBool("sway", true);
    cfg.batterySaver = prefs.getBool("batsave", true);
    cfg.txPowerDbm = prefs.getChar("txdbm", appcfg::kBatteryTxPowerDbm);
    cfg.webServerEnabled = prefs.getBool("websrv", true);
    cfg.otaPassword = prefs.getString("otapw", "");
    return cfg;
}

bool ConfigStore::save(const DeviceConfig& cfg) {
    if (!ready_) return false;
    prefs.putString("ssid", cfg.wifiSsid);
    prefs.putString("pass", cfg.wifiPassword);
    prefs.putString("host", cfg.serverHost);
    prefs.putUShort("port", cfg.serverPort);
    prefs.putString("path", cfg.serverPath);
    prefs.putString("name", cfg.deviceName);
    prefs.putString("pack", cfg.packName);
    prefs.putUChar("vol", cfg.volume);
    prefs.putUChar("bright", cfg.brightness);
    prefs.putUChar("led", cfg.ledBrightness);
    prefs.putBool("sway", cfg.swayEnabled);
    prefs.putBool("batsave", cfg.batterySaver);
    prefs.putChar("txdbm", cfg.txPowerDbm);
    prefs.putBool("websrv", cfg.webServerEnabled);
    prefs.putString("otapw", cfg.otaPassword);
    return prefs.getString("ssid", "") == cfg.wifiSsid;
}

void ConfigStore::clear() {
    if (ready_) prefs.clear();
}

bool ConfigStore::hasWifi() {
    return ready_ && prefs.getString("ssid", "").length() > 0;
}

bool ConfigStore::mergeFromSd(DeviceConfig& cfg) {
    const char* path = "/companion/config/device.json";
    if (!SD.exists(path)) return false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    StaticJsonDocument<768> doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        log_w("device.json parse failed: %s", err.c_str());
        return false;
    }

    bool changed = false;
    auto takeString = [&](const char* key, String& target) {
        const char* v = doc[key] | "";
        if (*v && target != v) {
            target = v;
            changed = true;
        }
    };
    takeString("wifi_ssid", cfg.wifiSsid);
    takeString("wifi_password", cfg.wifiPassword);
    takeString("server_host", cfg.serverHost);
    takeString("server_path", cfg.serverPath);
    takeString("device_name", cfg.deviceName);
    takeString("pack", cfg.packName);
    if (doc.containsKey("server_port")) {
        const uint16_t port = doc["server_port"];
        if (port && port != cfg.serverPort) {
            cfg.serverPort = port;
            changed = true;
        }
    }
    if (doc.containsKey("volume")) cfg.volume = doc["volume"];
    if (doc.containsKey("brightness")) cfg.brightness = doc["brightness"];
    if (doc.containsKey("led_brightness")) cfg.ledBrightness = doc["led_brightness"];
    // "pack_server" is what this was called when the page only did packs;
    // cards written before the rename still say it.
    if (doc.containsKey("pack_server")) cfg.webServerEnabled = doc["pack_server"];
    if (doc.containsKey("web_server")) cfg.webServerEnabled = doc["web_server"];
    takeString("ota_password", cfg.otaPassword);
    if (doc.containsKey("battery_saver")) cfg.batterySaver = doc["battery_saver"];
    if (doc.containsKey("tx_power_dbm")) cfg.txPowerDbm = doc["tx_power_dbm"];

    if (changed) save(cfg);
    return changed;
}
