#include "NetworkManager.hpp"

#include "Board.hpp"
#include "ResetReason.hpp"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_wifi.h>

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
    // After begin(), because the driver has to be started before it will
    // accept this - set earlier it is silently ignored.
    if (wantTxDbm_) setTxPower(wantTxDbm_);
}

void NetworkManager::setTxPower(int8_t dbm) {
    wantTxDbm_ = dbm;
    if (!dbm) return;
    // The API works in quarter-dBm and the part tops out at 20.
    const int8_t quarters = static_cast<int8_t>(dbm < 2 ? 8 : dbm > 20 ? 80 : dbm * 4);
    const esp_err_t err = esp_wifi_set_max_tx_power(quarters);
    if (err != ESP_OK) {
        log_w("wifi: tx power %d dBm refused (%d)", dbm, err);
        return;
    }
    log_i("wifi: tx power capped at %d dBm", dbm);
}

int8_t NetworkManager::txPower() const {
    int8_t quarters = 0;
    if (esp_wifi_get_max_tx_power(&quarters) != ESP_OK) return 0;
    return static_cast<int8_t>(quarters / 4);
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
        // No client-side heartbeat. The server pings every 20 seconds, which
        // is what keeps the link alive and detects a dead peer; running a
        // second heartbeat on a part with 80 KB of heap only adds a way for
        // the connection to be torn down and rebuilt, and each rebuild costs
        // memory that is not fully returned.
        wsStarted_ = true;
    }
    if (wsStarted_) ws_.loop();
}

void NetworkManager::wsThunk(WStype_t type, uint8_t* payload, size_t length) {
    if (instance_) instance_->onWsEvent(type, payload, length);
}

void NetworkManager::onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    log_i("ws event %d len=%u heap=%u", (int)type, (unsigned)length,
          (unsigned)ESP.getFreeHeap());
    switch (type) {
        case WStype_CONNECTED:
            wsConnected_ = true;
            uplinkStalled_ = false;
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

void NetworkManager::setSerialLink(bool on) {
    if (serialLink_ == on) return;
    serialLink_ = on;
    if (on) sendHello();
}

void NetworkManager::emitText(const char* json, size_t length) {
    if (serialLink_) {
        // Length prefixed, because the same port carries log lines and the
        // host must know where a frame ends without scanning for a delimiter.
        Serial.printf("@tx %u\n", static_cast<unsigned>(length));
        Serial.write(reinterpret_cast<const uint8_t*>(json), length);
    }
    if (wsConnected_) ws_.sendTXT(json, length);
}

void NetworkManager::sendHello() {
    if (!serverConnected()) return;
    StaticJsonDocument<448> doc;
    doc["type"] = "hello";
    doc["device"] = board::kDeviceType;
    doc["name"] = cfg_.deviceName;
    doc["protocol"] = protocol::kProtocolVersion;
    doc["fw"] = M5COMPANION_VERSION;
    doc["audio_format"] = protocol::kAudioFormat;
    doc["audio_rate"] = protocol::kAudioRate;
    doc["chunk_samples"] = appcfg::kAudioSamplesPerChunk;
    doc["ip"] = ip_;
    // Why the last boot happened. The interesting failures are the ones with
    // the cable out, where the serial console is not there to say.
    doc["reset"] = appdiag::resetReasonName();
    String out;
    serializeJson(doc, out);
    const bool ok = wsConnected_ ? ws_.sendTXT(out) : true;
    log_i("hello %u bytes -> %s", (unsigned)out.length(), ok ? "sent" : "FAILED");
    if (serialLink_) emitText(out);
}

void NetworkManager::sendListenBegin() {
    if (!serverConnected()) return;
    uplinkChunks_ = 0;
    uplinkFailures_ = 0;
    uplinkStalled_ = false;
    // The rate the microphone actually runs at, which is not the rate the
    // speaker does: the ADC path tops out below 16 kHz. The server resamples.
    char frame[96];
    snprintf(frame, sizeof(frame),
             "{\"type\":\"listen.begin\",\"format\":\"pcm_s16le\",\"rate\":%u}",
             (unsigned)appcfg::kMicSampleRate);
    emitText(String(frame));
}

void NetworkManager::sendListenEnd(bool cancelled) {
    if (!serverConnected()) return;
    // What actually left the device, against what the microphone produced.
    // A short utterance at the server can mean a short press, a starved
    // uplink or a full queue, and those need different fixes.
    log_i("uplink: %u chunks sent, %u sends failed (%.2f s of audio)",
          (unsigned)uplinkChunks_, (unsigned)uplinkFailures_,
          uplinkChunks_ * appcfg::kAudioSamplesPerChunk / (float)appcfg::kAudioSampleRate);
    emitText(String(cancelled ? "{\"type\":\"listen.end\",\"cancelled\":true}"
                              : "{\"type\":\"listen.end\"}"));
}

void NetworkManager::sendState(CompanionState state, Expression expression) {
    if (!serverConnected()) return;
    StaticJsonDocument<192> doc;
    doc["type"] = "device.state";
    doc["state"] = stateName(state);
    doc["expression"] = expressionName(expression);
    String out;
    serializeJson(doc, out);
    emitText(out);
}

void NetworkManager::sendTelemetry(uint8_t battery, bool charging, uint32_t freeHeap,
                                   uint32_t fpsX10, uint16_t droppedChunks) {
    if (!serverConnected()) return;
    StaticJsonDocument<256> doc;
    doc["type"] = "device.telemetry";
    doc["battery"] = battery;
    doc["charging"] = charging;
    doc["heap"] = freeHeap;
    doc["fps"] = fpsX10 / 10.0f;
    doc["rssi"] = rssi();
    // Audio chunks the playback queue had no room for. Anything but zero
    // during a reply means the server is sending faster than real time.
    doc["dropped"] = droppedChunks;
    String out;
    serializeJson(doc, out);
    emitText(out);
}

bool NetworkManager::sendAudio(const int16_t* samples, size_t sampleCount) {
    if (!samples || !sampleCount) return false;
    const size_t bytes = sampleCount * sizeof(int16_t);
    if (serialLink_) {
        Serial.printf("@txb %u\n", static_cast<unsigned>(bytes));
        Serial.write(reinterpret_cast<const uint8_t*>(samples), bytes);
    }
    if (wsConnected_) {
        // A send that cannot go through costs the library's whole write
        // timeout, and the uplink asks for fifty of these a second. Paying the
        // timeout on every one turns a dead socket into a device that answers
        // its buttons once every few seconds - which is what "it froze showing
        // listening" was. Once one send has failed, the rest of the utterance
        // is dropped without asking again; the next connection clears it.
        if (uplinkStalled_) {
            ++uplinkFailures_;
            return false;
        }
        const bool ok = ws_.sendBIN(reinterpret_cast<const uint8_t*>(samples), bytes);
        if (ok) {
            ++uplinkChunks_;
        } else {
            ++uplinkFailures_;
            uplinkStalled_ = true;
            log_w("uplink stalled after %u chunks; dropping the rest",
                  (unsigned)uplinkChunks_);
        }
        return ok;
    }
    if (serialLink_) ++uplinkChunks_;
    return serialLink_;
}
