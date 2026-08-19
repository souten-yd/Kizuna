#include "AudioManager.hpp"

#include <M5Unified.h>

bool AudioManager::begin() {
    micQueue_ = xQueueCreate(appcfg::kMicQueueDepth, sizeof(Chunk));
    spkQueue_ = xQueueCreate(appcfg::kPlaybackQueueDepth, sizeof(Chunk));
    if (!micQueue_ || !spkQueue_) return false;

    M5.Speaker.setVolume(appcfg::kSpeakerVolume);
    M5.Speaker.end();
    M5.Mic.end();

    // Core 0 keeps audio away from the display task's SPI bursts on core 1.
    return xTaskCreatePinnedToCore(taskThunk, "audio", 4096, this, 4, &task_, 0) == pdPASS;
}

void AudioManager::requestCapture() { requested_ = Mode::Capture; }
void AudioManager::requestPlayback() { requested_ = Mode::Playback; }
void AudioManager::requestIdle() { requested_ = Mode::Idle; }

void AudioManager::setMuted(bool muted) {
    muted_ = muted;
    if (mode_ == Mode::Playback) M5.Speaker.setVolume(muted ? 0 : appcfg::kSpeakerVolume);
}

bool AudioManager::playbackDrained() const {
    return spkQueue_ && uxQueueMessagesWaiting(spkQueue_) == 0 && !M5.Speaker.isPlaying();
}

bool AudioManager::pushPlayback(const uint8_t* data, size_t bytes) {
    if (!spkQueue_ || !data || bytes < sizeof(int16_t)) return false;

    // Frames larger than one chunk are split rather than truncated, so a
    // server that sends 40 ms or 100 ms packets still works.
    size_t offset = 0;
    bool ok = true;
    while (offset < bytes) {
        Chunk chunk;
        const size_t remaining = bytes - offset;
        const size_t take = min(remaining, appcfg::kAudioBytesPerChunk);
        chunk.samples = static_cast<uint16_t>(take / sizeof(int16_t));
        if (!chunk.samples) break;
        memcpy(chunk.data, data + offset, chunk.samples * sizeof(int16_t));
        if (xQueueSend(spkQueue_, &chunk, pdMS_TO_TICKS(20)) != pdTRUE) {
            ++dropped_;
            ok = false;
            break;
        }
        offset += chunk.samples * sizeof(int16_t);
    }
    return ok;
}

bool AudioManager::popCapture(Chunk& out) {
    return micQueue_ && xQueueReceive(micQueue_, &out, 0) == pdTRUE;
}

uint8_t AudioManager::levelFrom(const int16_t* samples, size_t count) {
    if (!samples || !count) return 0;
    uint32_t peak = 0;
    uint64_t energy = 0;
    for (size_t i = 0; i < count; i += 2) {
        const uint32_t v = static_cast<uint32_t>(abs(samples[i]));
        if (v > peak) peak = v;
        energy += static_cast<uint64_t>(v) * v;
    }
    const uint32_t rms = static_cast<uint32_t>(sqrt(static_cast<double>(energy / ((count + 1) / 2))));

    // Blend RMS with peak so plosives open the mouth without a sustained
    // vowel pinning it wide.
    const uint32_t mixed = (rms * 3 + peak) / 4;
    if (mixed < 300) return 0;
    if (mixed < 900) return 1;
    if (mixed < 2200) return 2;
    if (mixed < 5000) return 3;
    return 4;
}

void AudioManager::applyMode(Mode target) {
    if (mode_ == target) return;

    switch (mode_) {
        case Mode::Capture:
            M5.Mic.end();
            captureWarm_ = false;
            break;
        case Mode::Playback:
            M5.Speaker.stop();
            M5.Speaker.end();
            xQueueReset(spkQueue_);
            prerolled_ = false;
            break;
        default:
            break;
    }

    lipLevel_ = 0;

    switch (target) {
        case Mode::Capture:
            xQueueReset(micQueue_);
            M5.Mic.begin();
            captureIdx_ = 0;
            captureWarm_ = false;
            break;
        case Mode::Playback:
            M5.Speaker.begin();
            M5.Speaker.setVolume(muted_ ? 0 : appcfg::kSpeakerVolume);
            prerolled_ = false;
            break;
        default:
            break;
    }
    mode_ = target;
}

void AudioManager::serviceCapture() {
    if (!M5.Mic.isEnabled()) return;

    Chunk& target = capture_[captureIdx_];
    target.samples = appcfg::kAudioSamplesPerChunk;
    if (!M5.Mic.record(target.data, appcfg::kAudioSamplesPerChunk, appcfg::kAudioSampleRate)) {
        vTaskDelay(1);
        return;
    }

    // record() fills asynchronously, so the buffer that is safe to read is the
    // one handed to the driver on the previous call.
    const uint8_t readyIdx = captureIdx_ ^ 1;
    captureIdx_ = readyIdx;
    if (!captureWarm_) {
        captureWarm_ = true;
        return;
    }

    Chunk& ready = capture_[readyIdx];
    lipLevel_ = levelFrom(ready.data, ready.samples);
    if (xQueueSend(micQueue_, &ready, 0) != pdTRUE) ++dropped_;
}

void AudioManager::servicePlayback() {
    const UBaseType_t waiting = uxQueueMessagesWaiting(spkQueue_);
    if (!prerolled_) {
        if (waiting < appcfg::kPlaybackPrerollChunks) {
            vTaskDelay(pdMS_TO_TICKS(4));
            return;
        }
        prerolled_ = true;
    }

    // Keep one chunk playing and one queued behind it: that is what M5Unified
    // needs to produce a seam-free stream.
    while (M5.Speaker.isPlaying(0) < 2) {
        Chunk chunk;
        if (xQueueReceive(spkQueue_, &chunk, 0) != pdTRUE) break;
        lipLevel_ = levelFrom(chunk.data, chunk.samples);
        M5.Speaker.playRaw(chunk.data, chunk.samples, appcfg::kAudioSampleRate, false, 1, 0, false);
    }

    if (!waiting && !M5.Speaker.isPlaying()) {
        lipLevel_ = 0;
        // Underrun: re-arm the jitter buffer so a network hiccup does not turn
        // into a machine-gun stutter.
        prerolled_ = false;
    }
    vTaskDelay(pdMS_TO_TICKS(4));
}

void AudioManager::run() {
    for (;;) {
        if (requested_ != mode_) applyMode(requested_);

        switch (mode_) {
            case Mode::Capture:
                serviceCapture();
                break;
            case Mode::Playback:
                servicePlayback();
                break;
            default:
                lipLevel_ = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
        }
    }
}

void AudioManager::taskThunk(void* ctx) {
    static_cast<AudioManager*>(ctx)->run();
}
