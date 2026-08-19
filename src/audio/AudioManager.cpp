#include "AudioManager.hpp"

#include <math.h>

#include <driver/adc.h>
#include <esp_timer.h>

#include <M5Unified.h>

bool AudioManager::begin() {
    micQueue_ = xQueueCreate(appcfg::kMicQueueDepth, sizeof(Chunk));
    spkQueue_ = xQueueCreate(appcfg::kPlaybackQueueDepth, sizeof(Chunk));
    if (!micQueue_ || !spkQueue_) return false;

    M5.Speaker.setVolume(appcfg::kSpeakerVolume);
    M5.Speaker.end();

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
    log_i("audio mode -> %d (produced %u chunks, %u refusals, %u us in record())",
          (int)target, (unsigned)producedChunks_, (unsigned)recordFailures_,
          (unsigned)recordMicros_);
    producedChunks_ = 0;
    recordFailures_ = 0;
    recordMicros_ = 0;
    if (mode_ == target) return;

    switch (mode_) {
        case Mode::Capture:
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
            beginAdcMic();
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

void AudioManager::selfTest(uint16_t chunks, uint8_t overSampling, uint16_t dmaLen,
                            uint8_t dmaCount, uint8_t gainShift) {
    (void)overSampling;
    (void)dmaLen;
    (void)dmaCount;
    requestIdle();
    for (int i = 0; i < 50 && mode_ != Mode::Idle; ++i) vTaskDelay(pdMS_TO_TICKS(10));

    beginAdcMic();
    const uint8_t gain = gainShift ? gainShift : appcfg::kMicGainShift;
    Serial.printf("mic adc1_ch6 (GPIO34) rate=%u gain_shift=%u\n",
                  (unsigned)appcfg::kMicSampleRate, (unsigned)gain);

    static int16_t buf[appcfg::kMicSamplesPerChunk];
    uint32_t worst = 0;
    int32_t peak = 0;
    int64_t energy = 0;
    const uint32_t started = millis();
    for (uint16_t i = 0; i < chunks; ++i) {
        const uint32_t t0 = micros();
        captureAdc(buf, appcfg::kMicSamplesPerChunk, gain);
        const uint32_t spent = micros() - t0;
        if (spent > worst) worst = spent;
        for (size_t n = 0; n < appcfg::kMicSamplesPerChunk; ++n) {
            const int32_t v = buf[n];
            if (v > peak) peak = v;
            if (-v > peak) peak = -v;
            energy += static_cast<int64_t>(v) * v;
        }
    }
    const uint32_t elapsed = millis() - started;

    const uint32_t samples = static_cast<uint32_t>(chunks) * appcfg::kMicSamplesPerChunk;
    Serial.printf("{\"chunks\":%u,\"ms\":%u,\"effective_hz\":%u,\"worst_chunk_us\":%u,"
                  "\"peak\":%d,\"rms\":%u,\"dc\":%d}\n",
                  (unsigned)chunks, (unsigned)elapsed,
                  elapsed ? (unsigned)((uint64_t)samples * 1000 / elapsed) : 0,
                  (unsigned)worst, (int)peak,
                  (unsigned)(samples ? (uint32_t)sqrt((double)energy / samples) : 0),
                  (int)(dcBias_ >> 8));
}

void AudioManager::beginAdcMic() {
    // GPIO34 is ADC1 channel 6. 11 dB of attenuation puts the full 0-3.3 V
    // range in reach, which is what the electret's bias sits in the middle of.
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
    dcPrimed_ = false;
    dcBias_ = 0;
}

size_t AudioManager::captureAdc(int16_t* out, size_t count, uint8_t gainShift) {
    const int64_t period = 1000000 / appcfg::kMicSampleRate;
    int64_t next = esp_timer_get_time();
    for (size_t i = 0; i < count; ++i) {
        next += period;
        const int32_t raw = adc1_get_raw(ADC1_CHANNEL_6);

        // Track the bias rather than assuming half scale. The electret on this
        // base board sits nearer 3000 than 2048, and a wrong centre is a DC
        // step that the speaker reproduces as a thump.
        if (!dcPrimed_) {
            dcBias_ = raw << 8;
            dcPrimed_ = true;
        } else {
            dcBias_ += ((raw << 8) - dcBias_) >> 9;   // ~40 ms at 12 kHz
        }

        int32_t v = (raw - (dcBias_ >> 8)) << gainShift;
        if (v > INT16_MAX) v = INT16_MAX;
        if (v < INT16_MIN) v = INT16_MIN;
        out[i] = static_cast<int16_t>(v);

        // Spin rather than delay: the wait is 83 us and the tick is 1 ms. This
        // task is pinned to core 0 and the display owns core 1, so the cost is
        // one idle core during an utterance and nothing on the animation.
        while (esp_timer_get_time() < next) {
        }
    }
    return count;
}

void AudioManager::serviceCapture() {
    Chunk& target = capture_[captureIdx_];
    const uint32_t t0 = micros();
    target.samples = captureAdc(target.data, appcfg::kMicSamplesPerChunk,
                                appcfg::kMicGainShift);
    recordMicros_ += micros() - t0;
    captureIdx_ ^= 1;

    lipLevel_ = levelFrom(target.data, target.samples);
    ++producedChunks_;
    if (xQueueSend(micQueue_, &target, 0) != pdTRUE) ++dropped_;
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
        Chunk& slot = playback_[playbackIdx_];
        if (xQueueReceive(spkQueue_, &slot, 0) != pdTRUE) break;
        playbackIdx_ = (playbackIdx_ + 1) % kPlaybackSlots;
        lipLevel_ = levelFrom(slot.data, slot.samples);
        M5.Speaker.playRaw(slot.data, slot.samples, appcfg::kAudioSampleRate, false, 1, 0, false);
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
