#include "AudioManager.hpp"

#include <math.h>

#include <driver/adc.h>
#include <soc/i2s_struct.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>

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
            ++spkDropped_;
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
            releaseCore0();
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
            claimCore0();
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

void AudioManager::legacyMicClockReport(uint8_t overSampling) {
    requestIdle();
    for (int i = 0; i < 50 && mode_ != Mode::Idle; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    M5.Speaker.end();

    auto cfg = M5.Mic.config();
    cfg.pin_data_in = GPIO_NUM_34;
    cfg.use_adc = true;
    cfg.i2s_port = I2S_NUM_0;
    cfg.sample_rate = 16000;
    cfg.over_sampling = overSampling ? overSampling : 2;
    M5.Mic.config(cfg);
    if (!M5.Mic.begin()) {
        Serial.println("err mic begin failed");
        return;
    }
    // Let it settle and actually stream before reading the dividers back.
    static int16_t scratch[64];
    M5.Mic.record(scratch, 64, cfg.sample_rate);
    vTaskDelay(pdMS_TO_TICKS(200));

    const uint32_t n = I2S0.clkm_conf.clkm_div_num;
    const uint32_t a = I2S0.clkm_conf.clkm_div_a;
    const uint32_t b = I2S0.clkm_conf.clkm_div_b;
    const uint32_t bck = I2S0.sample_rate_conf.rx_bck_div_num;
    const uint32_t bits = I2S0.sample_rate_conf.rx_bits_mod;
    M5.Mic.end();

    // What the library asked calcClockDiv for, from its own arithmetic:
    // bits = 1 when use_adc, div_m = 8, so the base is 80 MHz / 8 = 10 MHz,
    // and the target is sample_rate * over_sampling.
    const uint32_t base = 80000000u / 8u;
    const uint32_t want = cfg.sample_rate * cfg.over_sampling;
    const float needed = (float)base / want;
    const float actual = (a > 0) ? (float)base / (n + (float)b / a) : (float)base / n;

    Serial.printf("{\"over\":%u,\"clkm_div_num\":%u,\"clkm_div_a\":%u,\"clkm_div_b\":%u,"
                  "\"rx_bck_div_num\":%u,\"rx_bits_mod\":%u,"
                  "\"divider_needed\":%.1f,\"i2s_clk_hz\":%.0f,\"raw_per_bck32_hz\":%.0f}\n",
                  (unsigned)cfg.over_sampling, (unsigned)n, (unsigned)a, (unsigned)b,
                  (unsigned)bck, (unsigned)bits, needed, actual, actual / 32.0f);
}

void AudioManager::beginAdcMic() {
    // GPIO34 is ADC1 channel 6. 11 dB of attenuation puts the full 0-3.3 V
    // range in reach, which is what the electret's bias sits in the middle of.
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);

    // The bias is where the electret idles, which is a property of the part and
    // not of the utterance, so it is measured once and kept. Re-priming it per
    // utterance took the very first sample as the centre; land that sample on a
    // peak - which is likely, because people start talking as they press - and
    // the centre is wrong by the amplitude of the speech, sixteen times the
    // error reaches the output, and it clips until the filter drags the bias
    // back. That takes about 43 ms at 12 kHz, which is a syllable. Transcripts
    // came back missing their first syllable: "konnichiwa" as "chiwa".
    if (!dcPrimed_) {
        int32_t sum = 0;
        constexpr int kPrimeSamples = 128;      // ~11 ms, and averaged
        for (int i = 0; i < kPrimeSamples; ++i) {
            sum += adc1_get_raw(ADC1_CHANNEL_6);
            esp_rom_delay_us(80);
        }
        dcBias_ = (sum / kPrimeSamples) << 8;
        dcPrimed_ = true;
    }
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

void AudioManager::claimCore0() {
    // Reading the ADC by hand means spinning between samples: at 12 kHz the
    // wait is 83 us and a tick is 1 ms, so there is no sleeping through it.
    // That leaves core 0's idle task with nothing, and the idle task is what
    // the watchdog watches - five seconds of held button and the device
    // reboots mid-sentence. It did, reproducibly, and the log named IDLE0.
    //
    // The idle task is excused for as long as the capture runs, and this task
    // takes its place under the watchdog: something still has to prove it is
    // alive, and a capture loop that stops feeding is exactly the failure the
    // watchdog is for.
    if (wdtHeld_) return;
    disableCore0WDT();
    esp_task_wdt_add(nullptr);
    wdtHeld_ = true;
}

void AudioManager::releaseCore0() {
    if (!wdtHeld_) return;
    esp_task_wdt_delete(nullptr);
    enableCore0WDT();
    wdtHeld_ = false;
}

void AudioManager::serviceCapture() {
    Chunk& target = capture_[captureIdx_];
    const uint32_t t0 = micros();
    target.samples = captureAdc(target.data, appcfg::kMicSamplesPerChunk,
                                appcfg::kMicGainShift);
    recordMicros_ += micros() - t0;
    captureIdx_ ^= 1;

    if (wdtHeld_) esp_task_wdt_reset();
    lipLevel_ = levelFrom(target.data, target.samples);
    ++producedChunks_;
    if (xQueueSend(micQueue_, &target, 0) != pdTRUE) ++micDropped_;
}

void AudioManager::servicePlayback() {
    const UBaseType_t waiting = uxQueueMessagesWaiting(spkQueue_);
    if (!prerolled_) {
        // Waiting for a full buffer is right in the middle of an utterance and
        // wrong at the end of one: the last chunks of a sentence are fewer
        // than the preroll, so holding out for more strands them. They then
        // play glued to the front of the next sentence, which is what a
        // clipped, hurried reply actually is.
        if (!streamEnded_ && waiting < appcfg::kPlaybackPrerollChunks) {
            vTaskDelay(pdMS_TO_TICKS(4));
            return;
        }
        if (!waiting) {
            vTaskDelay(pdMS_TO_TICKS(4));
            return;
        }
        prerolled_ = true;
    }

    // Keep one chunk playing and one queued behind it: each virtual channel
    // holds two, so isPlaying(0) returning 2 means there is no room. The
    // buffer handed to playRaw must stay put until the mixer has finished
    // reading it - see kPlaybackSlots.
    while (M5.Speaker.isPlaying(0) < 2) {
        Chunk& slot = playback_[playbackIdx_];
        if (xQueueReceive(spkQueue_, &slot, 0) != pdTRUE) break;

        // playRaw can still refuse, and the chunk is out of the queue by now:
        // putting it back is the difference between a gap and a lost word.
        if (!M5.Speaker.playRaw(slot.data, slot.samples, appcfg::kAudioSampleRate,
                                false, 1, 0, false)) {
            ++refused_;
            xQueueSendToFront(spkQueue_, &slot, 0);
            break;
        }
        playbackIdx_ = (playbackIdx_ + 1) % kPlaybackSlots;
        lipLevel_ = levelFrom(slot.data, slot.samples);
    }

    if (!waiting && !M5.Speaker.isPlaying()) {
        lipLevel_ = 0;
        if (underruns_ < UINT16_MAX) ++underruns_;
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
