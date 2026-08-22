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

    // The server has said the utterance is over. Everything still queued is
    // all there will ever be, so it must be played rather than waited on: the
    // jitter buffer's job ends here.
    void endStream() { streamEnded_ = true; }
    void beginStream() { streamEnded_ = false; }

    // Producer side: network -> speaker.
    bool pushPlayback(const uint8_t* data, size_t bytes);
    // Consumer side: mic -> network. Returns false when nothing is pending.
    bool popCapture(Chunk& out);

    void setMuted(bool muted);
    void setVolume(uint8_t volume);
    bool muted() const { return muted_; }
    bool running() const { return task_ != nullptr; }
    uint8_t lipLevel() const { return lipLevel_; }
    // Playback and capture drops sound nothing alike and have nothing to do
    // with each other: one is a gap in what you hear, the other a gap in what
    // the server heard. Counting them together made a report that could not be
    // acted on.
    uint16_t droppedChunks() const { return spkDropped_ + micDropped_; }
    uint16_t droppedPlayback() const { return spkDropped_; }
    uint16_t droppedCapture() const { return micDropped_; }
    uint16_t playbackUnderruns() const { return underruns_; }
    uint16_t playbackRefusals() const { return refused_; }

    // Records a burst straight from the driver and reports what it cost,
    // with the state machine, the queue and the network out of the way.
    void selfTest(uint16_t chunks, uint8_t overSampling, uint16_t dmaLen,
                  uint8_t dmaCount, uint8_t magnification);

    // Brings up M5Unified's I2S ADC microphone and reports the clock divider
    // the library ended up programming. Kept after that path was abandoned,
    // because the numbers are the evidence for an upstream bug report.
    void legacyMicClockReport(uint8_t overSampling);

private:
    enum class Mode : uint8_t { Idle, Capture, Playback };

    static void taskThunk(void* ctx);
    void run();
    void applyMode(Mode target);
    void serviceCapture();
    void beginAdcMic();
    size_t captureAdc(int16_t* out, size_t count, uint8_t gainShift);
    void claimCore0();
    void releaseCore0();
    void servicePlayback();
    static uint8_t levelFrom(const int16_t* samples, size_t count);

    QueueHandle_t micQueue_ = nullptr;
    QueueHandle_t spkQueue_ = nullptr;
    TaskHandle_t task_ = nullptr;

    volatile Mode mode_ = Mode::Idle;
    volatile Mode requested_ = Mode::Idle;
    volatile bool muted_ = false;
    volatile uint8_t volume_ = appcfg::kSpeakerVolume;
    volatile uint8_t lipLevel_ = 0;
    volatile uint16_t spkDropped_ = 0;
    volatile uint16_t micDropped_ = 0;
    volatile uint16_t underruns_ = 0;
    volatile uint16_t refused_ = 0;
    uint32_t producedChunks_ = 0;
    uint32_t recordFailures_ = 0;
    uint32_t recordMicros_ = 0;
    int32_t dcBias_ = 0;      // tracked, not assumed: the electret does not sit at 2048
    bool dcPrimed_ = false;
    bool prerolled_ = false;
    volatile bool streamEnded_ = false;

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
    bool wdtHeld_ = false;
};
