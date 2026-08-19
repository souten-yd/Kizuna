#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>

#include "AppTypes.hpp"
#include "app/EventBus.hpp"
#include "storage/ConfigStore.hpp"

// Wi-Fi + WebSocket transport.
//
// Everything here runs on the Arduino loop task. WebSocketsClient is not
// thread safe, so audio never touches it directly: the audio task hands PCM to
// the loop through a queue and the loop does the sending.
class NetworkManager {
public:
    using BinarySink = void (*)(const uint8_t* data, size_t len, void* ctx);

    void begin(const DeviceConfig& cfg, EventBus& events);
    void loop(uint32_t nowMs);

    bool wifiConnected() const;
    bool serverConnected() const { return wsConnected_ || serialLink_; }

    // The USB serial link: the same protocol, over the cable that already
    // flashes the board. Wi-Fi is the product, but a device on a bench with no
    // credentials still needs to reach a server, and a bridge on the host is
    // cheaper than a captive portal every time the network changes.
    void setSerialLink(bool on);
    bool serialLinkActive() const { return serialLink_; }
    void deliverText(const uint8_t* payload, size_t length) { handleText(payload, length); }
    void deliverBinary(const uint8_t* data, size_t length) {
        if (sink_ && length) sink_(data, length, sinkCtx_);
    }
    int8_t rssi() const;
    const char* ipAddress() const { return ip_.c_str(); }

    void setBinarySink(BinarySink sink, void* ctx) {
        sink_ = sink;
        sinkCtx_ = ctx;
    }

    void sendHello();
    void sendListenBegin();
    void sendListenEnd(bool cancelled = false);
    void sendState(CompanionState state, Expression expression);
    void sendTelemetry(uint8_t battery, bool charging, uint32_t freeHeap, uint32_t fpsX10,
                       uint16_t droppedChunks);
    bool sendAudio(const int16_t* samples, size_t sampleCount);

private:
    static void wsThunk(WStype_t type, uint8_t* payload, size_t length);
    void onWsEvent(WStype_t type, uint8_t* payload, size_t length);
    void handleText(const uint8_t* payload, size_t length);
    void startWifi();
    // Routes one outbound message to whichever transport is up.
    void emitText(const char* json, size_t length);
    void emitText(const String& json) { emitText(json.c_str(), json.length()); }

    static NetworkManager* instance_;

    DeviceConfig cfg_;
    EventBus* events_ = nullptr;
    WebSocketsClient ws_;
    BinarySink sink_ = nullptr;
    void* sinkCtx_ = nullptr;

    bool wsStarted_ = false;
    bool wsConnected_ = false;
    bool serialLink_ = false;
    bool wifiWasUp_ = false;
    String ip_;
    uint32_t lastWifiAttemptMs_ = 0;
    uint32_t lastTelemetryMs_ = 0;
};
