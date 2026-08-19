#include "NetworkManager.hpp"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "AppConfig.hpp"
#include "Protocol.hpp"

NetworkManager* NetworkManager::instance_ = nullptr;

void NetworkManager::begin(const DeviceConfig& cfg, EventBus& events) {
    cfg_ = cfg;
    events_ = &events;
    instance_ = this;

    if (cfg_.wifiSsid.isEmpty()) {
        log_w("no Wi-Fi credentials; staying offline");
        return;
    }
    startWifi();
}

void NetworkManager::startWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // voice latency beats the few mA saved here
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg_.wifiSsid.c_str(), cfg_.wifiPassword.c_str());
    lastWifiAttemptMs_ = millis();
}

bool NetworkManager::wifiConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

int8_t NetworkManager::rssi() const {
    return wifiConnected() ? static_cast<int8_t>(WiFi.RSSI()) : 0;
}

void NetworkManager::loop(uint32_t nowMs) {
    const bool up = wifiConnected();

    if (up != wifiWasUp_) {
        wifiWasUp_ = up;
        ip_ = up ? WiFi.localIP().toString() : String();
        if (events_) {
            events_->post(AppEvent(up ? AppEventType::NetworkConnected
                                      : AppEventType::NetworkDisconnected));
        }
        if (!up && wsConnected_) {
            wsConnected_ = false;
            if (events_) events_->post(AppEvent(AppEventType::ServerDisconnected));
        }
    }

    if (!up) {
        // Retry periodically without ever blocking the animation loop.
        if (!cfg_.wifiSsid.isEmpty() && nowMs - lastWifiAttemptMs_ > 20000) {
            log_i("Wi-Fi retry");
            WiFi.disconnect();
            startWifi();
        }
        return;
    }

    if (!wsStarted_ && !cfg_.serverHost.isEmpty()) {
        ws_.begin(cfg_.serverHost.c_str(), cfg_.serverPort, cfg_.serverPath.c_str());
        ws_.onEvent(wsThunk);
        ws_.setReconnectInterval(appcfg::kWsReconnectMs);
        ws_.enableHeartbeat(appcfg::kWsPingIntervalMs, 3000, 2);
        wsStarted_ = true;
    }
    if (wsStarted_) ws_.loop();
}

void NetworkManager::wsThunk(WStype_t type, uint8_t* payload, size_t length) {
    if (instance_) instance_->onWsEvent(type, payload, length);
}

void NetworkManager::onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsConnected_ = true;
            if (events_) events_->post(AppEvent(AppEventType::ServerConnected));
            sendHello();
            break;
        case WStype_DISCONNECTED:
            if (wsConnected_ && events_) events_->post(AppEvent(AppEventType::ServerDisconnected));
            wsConnected_ = false;
            break;
        case WStype_TEXT:
            handleText(payload, length);
            break;
        case WStype_BIN:
            if (sink_ && length) sink_(payload, length, sinkCtx_);
            break;
        case WStype_ERROR:
            log_w("websocket error");
            break;
        default:
            break;
    }
}

void NetworkManager::handleText(const uint8_t* payload, size_t length) {
    StaticJsonDocument<640> doc;
    if (deserializeJson(doc, payload, length) || !events_) return;

    const char* type = doc["type"] | "";

    if (!strcmp(type, "state")) {
        const char* state = doc["state"] | "";
        if (!strcmp(state, "thinking")) {
            events_->post(AppEvent(AppEventType::ServerThinking));
        } else if (!strcmp(state, "idle")) {
            events_->post(AppEvent(AppEventType::ServerIdle));
        }
        const char* expression = doc["expression"] | "";
        if (*expression) {
            events_->post(AppEvent(AppEventType::ServerExpression,
                                   static_cast<int32_t>(protocol::expressionFromString(expression)),
                                   doc["duration_ms"] | 1600));
        }
    } else if (!strcmp(type, "speech.begin")) {
        events_->post(AppEvent(AppEventType::SpeechBegin));
    } else if (!strcmp(type, "speech.end")) {
        events_->post(AppEvent(AppEventType::SpeechEnd));
    } else if (!strcmp(type, "expression")) {
        events_->post(AppEvent(AppEventType::ServerExpression,
                               static_cast<int32_t>(
                                   protocol::expressionFromString(doc["name"] | "neutral")),
                               doc["duration_ms"] | 1600));
    } else if (!strcmp(type, "gaze")) {
        events_->post(AppEvent(AppEventType::ServerGaze, doc["x"] | 0, 0, doc["y"] | 0));
    } else if (!strcmp(type, "error")) {
        events_->post(AppEvent(AppEventType::FatalError));
    }
}

void NetworkManager::sendHello() {
    if (!wsConnected_) return;
    StaticJsonDocument<448> doc;
    doc["type"] = "hello";
    doc["device"] = "m5go";
    doc["name"] = cfg_.deviceName;
    doc["protocol"] = protocol::kProtocolVersion;
    doc["fw"] = M5COMPANION_VERSION;
    doc["audio_format"] = protocol::kAudioFormat;
    doc["audio_rate"] = protocol::kAudioRate;
    doc["chunk_samples"] = appcfg::kAudioSamplesPerChunk;
    doc["ip"] = ip_;
    String out;
    serializeJson(doc, out);
    ws_.sendTXT(out);
}

void NetworkManager::sendListenBegin() {
    if (!wsConnected_) return;
    ws_.sendTXT("{\"type\":\"listen.begin\",\"format\":\"pcm_s16le\",\"rate\":16000}");
}

void NetworkManager::sendListenEnd(bool cancelled) {
    if (!wsConnected_) return;
    ws_.sendTXT(cancelled ? "{\"type\":\"listen.end\",\"cancelled\":true}"
                          : "{\"type\":\"listen.end\"}");
}

void NetworkManager::sendState(CompanionState state, Expression expression) {
    if (!wsConnected_) return;
    StaticJsonDocument<192> doc;
    doc["type"] = "device.state";
    doc["state"] = stateName(state);
    doc["expression"] = expressionName(expression);
    String out;
    serializeJson(doc, out);
    ws_.sendTXT(out);
}

void NetworkManager::sendTelemetry(uint8_t battery, bool charging, uint32_t freeHeap,
                                   uint32_t fpsX10) {
    if (!wsConnected_) return;
    StaticJsonDocument<256> doc;
    doc["type"] = "device.telemetry";
    doc["battery"] = battery;
    doc["charging"] = charging;
    doc["heap"] = freeHeap;
    doc["fps"] = fpsX10 / 10.0f;
    doc["rssi"] = rssi();
    String out;
    serializeJson(doc, out);
    ws_.sendTXT(out);
}

bool NetworkManager::sendAudio(const int16_t* samples, size_t sampleCount) {
    if (!wsConnected_ || !samples || !sampleCount) return false;
    return ws_.sendBIN(reinterpret_cast<const uint8_t*>(samples), sampleCount * sizeof(int16_t));
}
