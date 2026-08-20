#include "App.hpp"

#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <esp_system.h>

#include "AppConfig.hpp"
#include "network/PackWebServer.hpp"
#include "network/ProvisioningPortal.hpp"

void App::binarySinkThunk(const uint8_t* data, size_t len, void* ctx) {
    auto* self = static_cast<App*>(ctx);
    if (self) self->audio_.pushPlayback(data, len);
}

namespace {
// Mounts the card early, before the display task claims the SPI bus, purely so
// the config file can be read. Leaving it mounted is deliberate: the display
// task's SdCard::mount() then finds it already up and reuses it.
bool mountCardForConfig() {
    for (uint32_t freq : {appcfg::kSdFreqHz, 10000000U, 4000000U}) {
        if (SD.begin(appcfg::kSdCsPin, SPI, freq, "/sd", appcfg::kSdMaxOpenFiles)) return true;
        SD.end();
        delay(20);
    }
    return false;
}
}  // namespace

void App::maybeProvision() {
    // Setup is opt-in, entered by holding B+C through power-on.
    //
    // The earlier version dropped straight into the portal whenever Wi-Fi was
    // unconfigured, which meant a brand new device spent its first five
    // minutes showing a captive-portal screen instead of the character. An
    // unconfigured companion is still a companion; it just runs offline, and
    // the status bar says so.
    if (!(M5.BtnB.isPressed() && M5.BtnC.isPressed())) return;

    M5.Display.setRotation(1);
    ProvisioningPortal portal;
    if (portal.run(configStore_, configStore_.load())) {
        delay(300);
        ESP.restart();
    }
}

void App::begin() {
    auto cfg = M5.config();
    cfg.internal_spk = true;
    cfg.internal_mic = true;
    cfg.internal_imu = true;
    cfg.output_power = true;
    M5.begin(cfg);
    M5.Display.setRotation(1);

    // The buffer size has to be set before begin(); afterwards it is ignored.
    // A bigger receive buffer is what makes a 921600 baud file push survive
    // the gaps while a block is written to the card.
    Serial.setRxBufferSize(8192);
    Serial.begin(appcfg::kSerialBaud);
    delay(60);
    randomSeed(esp_random());

    M5.update();
    events_.begin();
    configStore_.begin();

    // Card first: a device.json on the SD card is the fastest way to provision
    // a unit that is sitting next to its own card reader, and it means the
    // captive portal is only needed when there is no card at all.
    config_ = configStore_.load();
    const bool card = mountCardForConfig();
    if (card && configStore_.mergeFromSd(config_)) {
        Serial.println("[config] applied /companion/config/device.json");
    }

    maybeProvision();
    config_ = configStore_.load();

    const uint32_t now = millis();
    state_.begin(now);
    director_.begin(now);
    director_.setSwayFrameCount(1);
    input_.begin(now);
    power_.begin(config_.brightness);
    leds_.begin(config_.ledBrightness);
    nextAmbientMs_ = now + random(appcfg::kAmbientGestureMinMs, appcfg::kAmbientGestureMaxMs);

    display_.setPackName(config_.packName.c_str());
    display_.setHeapReserve(config_.hasWifi() ? appcfg::kHeapReserveWifi
                                             : appcfg::kHeapReserveOffline);
    if (!display_.begin()) log_e("display task failed to start");
    if (!audio_.begin()) log_e("audio task failed to start");
    audio_.setMuted(false);
    M5.Speaker.setVolume(config_.volume);

    console_.begin(&display_, &configStore_, &config_, &network_, &audio_, &leds_);

    network_.setBinarySink(binarySinkThunk, this);
    network_.begin(config_, events_);

    Serial.printf("\n[M5Companion %s] heap=%u pack=%s server=%s:%u%s\n", M5COMPANION_VERSION,
                  ESP.getFreeHeap(), config_.packName.c_str(), config_.serverHost.c_str(),
                  config_.serverPort, config_.serverPath.c_str());
    if (!config_.hasWifi()) {
        Serial.println("[M5Companion] no Wi-Fi configured - running offline. "
                       "Edit /companion/config/device.json on the card, or hold B+C at "
                       "power-on for the setup portal.");
    }
}

void App::factoryReset() {
    configStore_.clear();
    display_.showMessage("Reset", "settings cleared", "rebooting...");
    leds_.off();
    delay(1200);
    ESP.restart();
}

void App::handleEvent(const AppEvent& event, uint32_t nowMs) {
    switch (event.type) {
        case AppEventType::PttPressed:
            if (state_.state() != CompanionState::Sleep) {
                audio_.requestCapture();
                network_.sendListenBegin();
            }
            director_.poke(nowMs);
            break;

        case AppEventType::PttReleased:
            if (audio_.capturing()) {
                audio_.requestIdle();
                network_.sendListenEnd();
            }
            break;

        case AppEventType::SpeechBegin:
            audio_.beginStream();
            speechEndPending_ = false;
            audio_.requestPlayback();
            break;

        case AppEventType::SpeechEnd:
            // Hold the SPEAKING pose until the jitter buffer has actually
            // drained, otherwise the mouth stops before the voice does.
            audio_.endStream();
            speechEndPending_ = true;
            return;

        case AppEventType::ServerGaze:
            director_.setTilt(static_cast<int8_t>(event.value), static_cast<int8_t>(event.value2));
            break;

        case AppEventType::MotionShake:
            director_.startle(nowMs);
            if (state_.state() == CompanionState::Idle) director_.triggerGesture(Gesture::Shake);
            break;

        case AppEventType::MotionPickup:
            director_.startle(nowMs);
            if (state_.state() == CompanionState::Idle) director_.triggerGesture(Gesture::TiltLeft);
            break;

        case AppEventType::ServerConnected:
            if (state_.state() != CompanionState::Sleep) director_.triggerGesture(Gesture::Nod);
            break;

        case AppEventType::SleepRequested:
            if (state_.state() != CompanionState::Sleep) director_.triggerGesture(Gesture::SleepPose);
            break;

        case AppEventType::NetworkDisconnected:
        case AppEventType::ServerDisconnected:
            if (audio_.playing() || audio_.capturing()) audio_.requestIdle();
            speechEndPending_ = false;
            break;

        default:
            break;
    }

    state_.handle(event, nowMs);
}

void App::serviceAudioUplink() {
    if (!audio_.capturing()) return;
    // Drain what is there. The old bound of four chunks a turn was justified by
    // leaving room for the animation, but the animation has its own task on the
    // other core - the bound was protecting nothing and costing speech. The
    // microphone produces fifty chunks a second, so four a turn needs this loop
    // to run twelve times a second come what may, and one slow websocket send
    // is enough for it not to.
    //
    // The cap that remains is against the opposite failure: a queue that has
    // somehow filled should not be emptied in one turn while everything else
    // waits.
    AudioManager::Chunk chunk;
    for (uint8_t i = 0; i < appcfg::kMicQueueDepth && audio_.popCapture(chunk); ++i) {
        network_.sendAudio(chunk.data, chunk.samples);
    }
}

void App::serviceButtons(uint32_t nowMs) {
    if (input_.factoryResetRequested()) factoryReset();

    if (input_.consumeMuteToggle()) {
        muted_ = !muted_;
        audio_.setMuted(muted_);
        state_.noteInteraction(nowMs);
        director_.poke(nowMs);
    }
    if (input_.consumeDebugToggle()) {
        debug_ = !debug_;
        director_.setDebug(debug_);
        DisplayTask::CommandMsg msg;
        msg.cmd = DisplayTask::Command::SetDebug;
        msg.value = debug_ ? 1 : 0;
        display_.post(msg);
    }
    if (input_.consumeBrightnessStep()) {
        config_.brightness = power_.cycleBrightness();
        configStore_.save(config_);
        state_.noteInteraction(nowMs);
    }
    if (input_.consumeSleepToggle()) {
        events_.post(AppEvent(state_.state() == CompanionState::Sleep
                                  ? AppEventType::WakeRequested
                                  : AppEventType::SleepRequested));
    }
}

void App::serviceAmbient(uint32_t nowMs) {
    // Ambient motion is deliberately idle-only: a full-screen gesture must
    // never interrupt listening, lip sync, an error, or a server-specified
    // transient expression. Storage is abundant; bus time is the scarce part.
    if (state_.state() != CompanionState::Idle || state_.expression() != Expression::Neutral ||
        audio_.playing() || audio_.capturing()) {
        if (static_cast<int32_t>(nowMs - nextAmbientMs_) >= 0) {
            nextAmbientMs_ = nowMs + random(appcfg::kAmbientGestureMinMs,
                                            appcfg::kAmbientGestureMaxMs);
        }
        return;
    }

    if (!nextAmbientMs_) {
        nextAmbientMs_ = nowMs + random(appcfg::kAmbientGestureMinMs,
                                        appcfg::kAmbientGestureMaxMs);
        return;
    }
    if (static_cast<int32_t>(nowMs - nextAmbientMs_) < 0) return;

    // Weighted toward subtle motion, with occasional comic poses. The source
    // frames and the transition order live in the character recipe; missing
    // clips on an older pack simply become a no-op in DisplayTask.
    const uint8_t pick = static_cast<uint8_t>(random(10));
    Expression expression = Expression::SoftSmile;
    Gesture gesture = Gesture::Nod;
    uint32_t holdMs = 1300;

    switch (pick) {
        case 0:
        case 1:
            expression = Expression::SoftSmile;
            gesture = Gesture::Nod;
            holdMs = 1300;
            break;
        case 2:
            expression = Expression::Curious;
            gesture = Gesture::TiltLeft;
            holdMs = 1600;
            break;
        case 3:
            expression = Expression::Curious;
            gesture = Gesture::TiltRight;
            holdMs = 1600;
            break;
        case 4:
            expression = Expression::Proud;
            gesture = Gesture::Lean;
            holdMs = 1400;
            break;
        case 5:
            expression = Expression::Peace;
            gesture = Gesture::LookAround;
            holdMs = 1700;
            break;
        case 6:
            expression = Expression::Cozy;
            gesture = Gesture::IdleMoment;
            holdMs = 1900;
            break;
        case 7:
            expression = Expression::Mischievous;
            gesture = Gesture::Shake;
            holdMs = 1300;
            break;
        case 8:
            expression = Expression::NodYes;
            gesture = Gesture::Nod;
            holdMs = 1200;
            break;
        default:
            expression = Expression::Cheerful;
            gesture = Gesture::Cheer;
            holdMs = 1500;
            break;
    }

    state_.setExpressionOverride(expression, holdMs, nowMs);
    director_.triggerGesture(gesture);
    nextAmbientMs_ = nowMs + random(appcfg::kAmbientGestureMinMs,
                                    appcfg::kAmbientGestureMaxMs);
}

void App::loop() {
    // A file transfer owns the SPI bus and the serial port; everything else
    // waits rather than fighting it.
    console_.poll();
    if (console_.busy()) return;

    M5.update();
    const uint32_t now = millis();

    network_.loop(now);

    // The pack manager only exists once there is a network to reach it on.
    if (network_.wifiConnected() && config_.packServerEnabled) {
        if (!web_.running()) web_.begin(&display_, &configStore_, &config_);
        web_.loop();
        if (web_.busy()) return;  // an upload owns the card
    }
    input_.update(events_, now);
    serviceButtons(now);

    AppEvent event;
    while (events_.poll(event)) handleEvent(event, now);

    serviceAudioUplink();

    if (speechEndPending_ && audio_.playbackDrained()) {
        speechEndPending_ = false;
        audio_.requestIdle();
        state_.handle(AppEvent(AppEventType::SpeechEnd), now);
        if (random(100) < 35) director_.triggerGesture(Gesture::Nod);
    }

    state_.setWifiOnline(network_.wifiConnected());
    state_.setServerOnline(network_.serverConnected());
    state_.update(now);
    serviceAmbient(now);

    // Once the pack is known, hand its sway length to the director so the body
    // loop matches the artwork rather than a compile-time guess.
    const uint8_t sway = config_.swayEnabled ? display_.swayFrames() : 1;
    director_.setSwayFrameCount(sway);

    director_.setState(state_.state());
    director_.setExpression(state_.expression());
    director_.setTilt(input_.gazeX(), input_.gazeY());
    director_.setLipLevel(audio_.lipLevel());
    director_.setDeviceStatus(power_.batteryPercent(), power_.charging(), network_.wifiConnected(),
                              network_.serverConnected(), muted_);
    display_.submit(director_.update(now));

    leds_.update(state_.state(), audio_.lipLevel(), network_.serverConnected(), muted_, now);
    power_.update(now, state_.state() == CompanionState::Sleep);

    if (state_.state() != lastReportedState_ && now - lastStatePushMs_ > 200) {
        lastReportedState_ = state_.state();
        lastStatePushMs_ = now;
        network_.sendState(state_.state(), state_.expression());
    }

    if (now - lastTelemetryMs_ > 15000) {
        lastTelemetryMs_ = now;
        DisplayTask::Stats stats;
        display_.snapshot(stats);
        network_.sendTelemetry(power_.batteryPercent(), power_.charging(), ESP.getFreeHeap(),
                               stats.fpsX10, audio_.droppedChunks());
        if (debug_) {
            Serial.printf("[stat] fps=%u.%u sd=%uB/s budget=%uB/s drawn=%uB/s cache=%u/%u heap=%u\n",
                          stats.fpsX10 / 10, stats.fpsX10 % 10, stats.sdBytesPerSec,
                          stats.budgetBytesPerSec, stats.drawnBytesPerSec, stats.cacheHits,
                          stats.cacheHits + stats.cacheMisses, ESP.getFreeHeap());
        }
    }

    delay(2);
}
