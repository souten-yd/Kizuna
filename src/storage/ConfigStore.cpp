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
    cfg.voiceTransport = prefs.getString("vtrans", "bridge");
    cfg.qnapHost = prefs.getString("qhost", "");
    cfg.qnapPort = prefs.getUShort("qport", 11435);
    cfg.qnapPath = prefs.getString("qpath", "/v1/voice/chat/stream?profile=m5go");
    cfg.deviceName = prefs.getString("name", "M5GO-Companion");
    cfg.packName = prefs.getString("pack", "kizuna");
    cfg.volume = prefs.getUChar("vol", appcfg::kSpeakerVolume);
    cfg.brightness = prefs.getUChar("bright", 160);
    cfg.ledBrightness = prefs.getUChar("led", appcfg::kLedBrightness);
    cfg.swayEnabled = prefs.getBool("sway", true);
    cfg.packServerEnabled = prefs.getBool("packsrv", false);
    return cfg;
}

bool ConfigStore::save(const DeviceConfig& cfg) {
    if (!ready_) return false;
    prefs.putString("ssid", cfg.wifiSsid);
    prefs.putString("pass", cfg.wifiPassword);
    prefs.putString("host", cfg.serverHost);
    prefs.putUShort("port", cfg.serverPort);
    prefs.putString("path", cfg.serverPath);
    prefs.putString("vtrans", cfg.voiceTransport);
    prefs.putString("qhost", cfg.qnapHost);
    prefs.putUShort("qport", cfg.qnapPort);
    prefs.putString("qpath", cfg.qnapPath);
    prefs.putString("name", cfg.deviceName);
    prefs.putString("pack", cfg.packName);
    prefs.putUChar("vol", cfg.volume);
    prefs.putUChar("bright", cfg.brightness);
    prefs.putUChar("led", cfg.ledBrightness);
    prefs.putBool("sway", cfg.swayEnabled);
    prefs.putBool("packsrv", cfg.packServerEnabled);
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
    StaticJsonDocument<1152> doc;
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
    takeString("voice_transport", cfg.voiceTransport);
    takeString("qnap_host", cfg.qnapHost);
    takeString("qnap_path", cfg.qnapPath);
    takeString("device_name", cfg.deviceName);
    takeString("pack", cfg.packName);
    if (doc.containsKey("server_port")) {
        const uint16_t port = doc["server_port"];
        if (port && port != cfg.serverPort) {
            cfg.serverPort = port;
            changed = true;
        }
    }
    if (doc.containsKey("qnap_port")) {
        const uint16_t port = doc["qnap_port"];
        if (port && port != cfg.qnapPort) {
            cfg.qnapPort = port;
            changed = true;
        }
    }
    if (doc.containsKey("volume")) cfg.volume = doc["volume"];
    if (doc.containsKey("brightness")) cfg.brightness = doc["brightness"];
    if (doc.containsKey("led_brightness")) cfg.ledBrightness = doc["led_brightness"];
    if (doc.containsKey("pack_server")) cfg.packServerEnabled = doc["pack_server"];

    if (!cfg.voiceTransport.equalsIgnoreCase("qnap_stream")) cfg.voiceTransport = "bridge";
    if (cfg.qnapPort == 0) cfg.qnapPort = 11435;
    if (cfg.qnapPath.isEmpty()) cfg.qnapPath = "/v1/voice/chat/stream?profile=m5go";

    if (changed) save(cfg);
    return changed;
}
