#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "AppConfig.hpp"

// Half-duplex audio for the internal analog mic (GPIO34) and 1 W speaker
// (GPIO25).
//
// M5GO cannot run both at once without howling, so the pipeline has exactly
// one owner at a time and the switch happens on the audio task itself - never
// from the caller's thread. Callers only ever set an intent.
class AudioManager {
public:
    struct Chunk {
        uint16_t samples = 0;
        // Sized for the microphone, which runs slower than playback.
        int16_t data[appcfg::kMicSamplesPerChunk > appcfg::kAudioSamplesPerChunk
                     ? appcfg::kMicSamplesPerChunk : appcfg::kAudioSamplesPerChunk];
    };

    bool begin();

    // Intents. Safe to call from any task.
    void requestCapture();
    void requestPlayback();
    void requestIdle();

    bool capturing() const { return mode_ == Mode::Capture; }
    bool playing() const { return mode_ == Mode::Playback; }
    bool playbackDrained() const;

    // Producer side: network -> speaker.
    bool pushPlayback(const uint8_t* data, size_t bytes);
    // Consumer side: mic -> network. Returns false when nothing is pending.
    bool popCapture(Chunk& out);

    void setMuted(bool muted);
    bool muted() const { return muted_; }
    uint8_t lipLevel() const { return lipLevel_; }
    uint16_t droppedChunks() const { return dropped_; }

    // Records a burst straight from the driver and reports what it cost,
    // with the state machine, the queue and the network out of the way.
    void selfTest(uint16_t chunks, uint8_t overSampling, uint16_t dmaLen,
                  uint8_t dmaCount, uint8_t magnification);

private:
    enum class Mode : uint8_t { Idle, Capture, Playback };

    static void taskThunk(void* ctx);
    void run();
    void applyMode(Mode target);
    void serviceCapture();
    void beginAdcMic();
    size_t captureAdc(int16_t* out, size_t count, uint8_t gainShift);
    void servicePlayback();
    static uint8_t levelFrom(const int16_t* samples, size_t count);

    QueueHandle_t micQueue_ = nullptr;
    QueueHandle_t spkQueue_ = nullptr;
    TaskHandle_t task_ = nullptr;

    volatile Mode mode_ = Mode::Idle;
    volatile Mode requested_ = Mode::Idle;
    volatile bool muted_ = false;
    volatile uint8_t lipLevel_ = 0;
    volatile uint16_t dropped_ = 0;
    uint32_t producedChunks_ = 0;
    uint32_t recordFailures_ = 0;
    uint32_t recordMicros_ = 0;
    int32_t dcBias_ = 0;      // tracked, not assumed: the electret does not sit at 2048
    bool dcPrimed_ = false;
    bool prerolled_ = false;

    Chunk capture_[2];
    uint8_t captureIdx_ = 0;

    // Playback buffers outlive the call that hands them over. M5Unified's
    // playRaw is asynchronous: it records the pointer and returns, and the
    // mixer task reads from it afterwards. A buffer on the caller's stack
    // is therefore overwritten while it is still being played, which sounds
    // like grit rather than like anything obviously broken. Four slots
    // against the two the mixer queues leaves a full round of margin.
    static constexpr uint8_t kPlaybackSlots = 4;
    Chunk playback_[kPlaybackSlots];
    uint8_t playbackIdx_ = 0;
    bool captureWarm_ = false;
};
