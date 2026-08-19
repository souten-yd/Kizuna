#include "AudioManager.hpp"

#include <math.h>

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
    log_i("audio mode -> %d (produced %u chunks, %u refusals, %u us in record())",
          (int)target, (unsigned)producedChunks_, (unsigned)recordFailures_,
          (unsigned)recordMicros_);
    producedChunks_ = 0;
    recordFailures_ = 0;
    recordMicros_ = 0;
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
            configureMic();
            M5.Mic.begin();
            {
                const auto& got = M5.Mic.config();
                log_i("mic: enabled=%d pin=%d adc=%d rate=%u mag=%u over=%u "
                      "i2s=%d dma=%ux%u",
                      (int)M5.Mic.isEnabled(), got.pin_data_in, (int)got.use_adc,
                      (unsigned)got.sample_rate, (unsigned)got.magnification,
                      (unsigned)got.over_sampling, (int)got.i2s_port,
                      (unsigned)got.dma_buf_len, (unsigned)got.dma_buf_count);
            }
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
                            uint8_t dmaCount, uint8_t magnification) {
    requestIdle();
    for (int i = 0; i < 50 && mode_ != Mode::Idle; ++i) vTaskDelay(pdMS_TO_TICKS(10));

    M5.Speaker.end();
    configureMic();
    {
        auto cfg = M5.Mic.config();
        if (overSampling) cfg.over_sampling = overSampling;
        if (dmaLen) cfg.dma_buf_len = dmaLen;
        if (dmaCount) cfg.dma_buf_count = dmaCount;
        if (magnification) cfg.magnification = magnification;
        M5.Mic.config(cfg);
    }
    if (!M5.Mic.begin()) {
        Serial.println("err mic begin failed");
        return;
    }
    const auto& cfg = M5.Mic.config();
    Serial.printf("mic pin=%d adc=%d rate=%u mag=%u over=%u i2s=%d dma=%ux%u\n",
                  cfg.pin_data_in, (int)cfg.use_adc, (unsigned)cfg.sample_rate,
                  (unsigned)cfg.magnification, (unsigned)cfg.over_sampling,
                  (int)cfg.i2s_port, (unsigned)cfg.dma_buf_len,
                  (unsigned)cfg.dma_buf_count);

    static int16_t buf[2][appcfg::kAudioSamplesPerChunk];
    uint8_t idx = 0;
    uint32_t worst = 0;
    int32_t peak = 0;
    int64_t energy = 0;
    const uint32_t started = millis();
    for (uint16_t i = 0; i < chunks; ++i) {
        const uint32_t t0 = micros();
        if (!M5.Mic.record(buf[idx], appcfg::kAudioSamplesPerChunk,
                           appcfg::kAudioSampleRate)) {
            Serial.printf("err record refused at chunk %u\n", (unsigned)i);
            break;
        }
        const uint32_t spent = micros() - t0;
        if (spent > worst) worst = spent;
        idx ^= 1;
        for (size_t n = 0; n < appcfg::kAudioSamplesPerChunk; ++n) {
            const int32_t v = buf[idx][n];
            if (v > peak) peak = v;
            if (-v > peak) peak = -v;
            energy += static_cast<int64_t>(v) * v;
        }
    }
    const uint32_t elapsed = millis() - started;
    M5.Mic.end();

    const uint32_t samples = static_cast<uint32_t>(chunks) * appcfg::kAudioSamplesPerChunk;
    Serial.printf("{\"chunks\":%u,\"ms\":%u,\"effective_hz\":%u,\"worst_record_us\":%u,"
                  "\"peak\":%d,\"rms\":%u}\n",
                  (unsigned)chunks, (unsigned)elapsed,
                  elapsed ? (unsigned)((uint64_t)samples * 1000 / elapsed) : 0,
                  (unsigned)worst, (int)peak,
                  (unsigned)(samples ? (uint32_t)sqrt((double)energy / samples) : 0));
}

void AudioManager::configureMic() {
    // M5Unified has no microphone configuration for board_M5Stack, because the
    // Core on its own does not have one. The M5GO's base board does: an analog
    // electret on GPIO34, read through the ADC. Calling M5.Mic.begin() without
    // setting this up starts a driver pointed at pins that are not connected,
    // which produces a couple of chunks a second instead of fifty and no
    // sound - and it fails silently, because record() keeps returning true.
    auto cfg = M5.Mic.config();
    cfg.pin_data_in = GPIO_NUM_34;
    cfg.use_adc = true;
    // ADC capture on the ESP32 is only available on I2S port 0, which is also
    // where the speaker's DAC lives. That is why this path is half duplex.
    cfg.i2s_port = I2S_NUM_0;
    cfg.sample_rate = appcfg::kAudioSampleRate;
    cfg.stereo = false;
    cfg.magnification = appcfg::kMicMagnification;
    cfg.over_sampling = 2;
    cfg.noise_filter_level = 0;
    M5.Mic.config(cfg);
}

void AudioManager::serviceCapture() {
    if (!M5.Mic.isEnabled()) {
        if (!micDisabledLogged_) {
            micDisabledLogged_ = true;
            log_w("capture: microphone reports disabled");
        }
        return;
    }
    micDisabledLogged_ = false;

    Chunk& target = capture_[captureIdx_];
    target.samples = appcfg::kAudioSamplesPerChunk;
    const uint32_t t0 = micros();
    const bool queued = M5.Mic.record(target.data, appcfg::kAudioSamplesPerChunk,
                                      appcfg::kAudioSampleRate);
    recordMicros_ += micros() - t0;
    if (!queued) {
        // The mic and the speaker share one I2S port on this board, so a
        // record that keeps refusing usually means the handover did not
        // complete rather than that the microphone is busy.
        if (++recordFailures_ % 200 == 1) {
            log_w("capture: record() refused %u times (produced %u chunks)",
                  (unsigned)recordFailures_, (unsigned)producedChunks_);
        }
        vTaskDelay(1);
        return;
    }
    recordFailures_ = 0;

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
    ++producedChunks_;
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
