#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app/EventBus.hpp"
#include "storage/ConfigStore.hpp"

// Direct QnapAssistant voice transport for memory-constrained M5GO units.
//
// Upload is HTTP/1.1 chunked PCM16: the whole utterance is never buffered on
// the ESP32. After PTT release the same connection becomes a multipart/mixed
// response. Each audio/wav part is stripped to PCM as it arrives and handed
// to the AudioManager queue in 20 ms slices, so neither the complete reply nor
// a base64 representation ever exists in heap.
class QnapStreamClient {
public:
    using BinarySink = void (*)(const uint8_t* data, size_t len, void* ctx);

    void begin(const DeviceConfig& cfg, EventBus* events, BinarySink sink, void* sinkCtx);
    void setNetworkAvailable(bool available);

    bool startUtterance();
    bool sendAudio(const int16_t* samples, size_t sampleCount);
    void finishUtterance(bool cancelled = false);
    void cancel();

    bool online() const { return online_; }
    bool busy() const { return phase_ != Phase::Idle; }
    uint32_t lastFirstAudioMs() const { return lastFirstAudioMs_; }
    uint32_t lastReplyMs() const { return lastReplyMs_; }

private:
    enum class Phase : uint8_t { Idle, Uploading, Receiving };

    static void responseTaskThunk(void* ctx);
    void receiveResponse();
    bool sendChunk(const uint8_t* data, size_t bytes);
    bool connectAndWriteHeaders();
    void fail(const char* reason, bool speechStarted = false);

    DeviceConfig cfg_;
    EventBus* events_ = nullptr;
    BinarySink sink_ = nullptr;
    void* sinkCtx_ = nullptr;
    WiFiClient client_;
    TaskHandle_t responseTask_ = nullptr;

    volatile Phase phase_ = Phase::Idle;
    volatile bool online_ = false;
    volatile bool networkAvailable_ = false;
    volatile bool abortRequested_ = false;
    uint32_t responseStartMs_ = 0;
    volatile uint32_t lastFirstAudioMs_ = 0;
    volatile uint32_t lastReplyMs_ = 0;
};
