#include "App.hpp"

#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

#include "AppConfig.hpp"
#include "Board.hpp"
#include "TouchUi.hpp"
#include "ResetReason.hpp"
#include "diag/BootLog.hpp"
#include "diag/LogRing.hpp"
#include "diag/Recovery.hpp"
#include "storage/FirmwareStore.hpp"
#include "network/DeviceWebServer.hpp"
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
    bool requested = M5.BtnB.isPressed() && M5.BtnC.isPressed();
    if constexpr (board::kHasTouch) {
        if (M5.Touch.getCount()) {
            const auto& touch = M5.Touch.getDetail();
            requested = touch.isPressed() &&
                        touchui::actionAt(touch.x, touch.y) == touchui::Action::Settings;
        } else {
            requested = false;
        }
    }
    if (!requested) return;

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

    // Before anything else logs anything, so the ring holds the whole boot
    // rather than whatever happened after the network came up.
    appdiag::LogRing::begin();

    // Before anything else can crash. The boot log goes first because its
    // verdict - how many of the last boots began after a crash - is half of
    // what decides whether this one runs reduced. Then Recovery counts the
    // boot and may not return at all: the way out of a boot loop is another
    // partition, and taking it means restarting.
    appdiag::BootLog::begin();
    appdiag::Recovery::begin(appdiag::BootLog::crashStreak());
    safeMode_ = appdiag::Recovery::safeMode();
    if (safeMode_) escalateRecovery();

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

    if (safeMode_) {
        // Nothing that reads the card, nothing that opens a codec, nothing
        // that allocates a tile cache. Whatever is wrong is most likely one of
        // those, and none of them is needed to be reachable.
        config_.webServerEnabled = true;
        showSafeModeScreen();
    } else {
        display_.setPackName(config_.packName.c_str());
        display_.setHeapReserve(config_.hasWifi() ? appcfg::kHeapReserveWifi
                                                 : appcfg::kHeapReserveOffline);
        if (!display_.begin()) log_e("display task failed to start");
        if (!audio_.begin()) log_e("audio task failed to start");
        audio_.setMuted(false);
        M5.Speaker.setVolume(config_.volume);
        audio_.setVolume(config_.volume);
        // The bar starts showing where the saved volume is, so the first tap
        // moves from something visible rather than appearing out of nowhere.
        director_.setVolume(volumeIndex(config_.volume), appcfg::kVolumeStepCount);
    }

    console_.begin(&display_, &configStore_, &config_, &network_, &audio_, &leds_, &events_,
                   &power_);

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

// What the device gives up when the cable comes out, and takes back when it
// returns. Applied on every change of charging state rather than once at boot,
// because the interesting transition is exactly the one the user makes by hand
// while watching.
void App::applyPowerPolicy(bool force) {
    if constexpr (!board::kNeedsBatteryLoadShedding) {
        if (!force && powerPolicyKnown_) return;
        powerPolicyKnown_ = true;
        batterySaver_ = false;
        power_.setActiveBrightness(config_.brightness);
        network_.setTxPower(20);
        log_i("power: CoreS3 full-power policy (M5GO load shedding disabled)");
        return;
    }
    const bool onBattery = !power_.charging();
    const bool want = onBattery && config_.batterySaver;
    if (!force && powerPolicyKnown_ && want == batterySaver_) return;
    powerPolicyKnown_ = true;
    batterySaver_ = want;

    // Zero stops the strip being clocked at all rather than clocking ten
    // pixels of zeroes, which is the part worth stopping.
    leds_.setBrightness(want ? 0 : config_.ledBrightness);
    power_.setActiveBrightness(want ? min(config_.brightness, appcfg::kBatteryBrightnessCap)
                                    : config_.brightness);
    network_.setTxPower(want ? config_.txPowerDbm : 20);

    log_i("power: %s (tx %d dBm, backlight %u, leds %s)",
          want ? "on battery - saving" : "on the cable - full",
          want ? config_.txPowerDbm : 20, power_.activeBrightness(),
          want ? "off" : "on");
}

// Whether this boot is going well enough to stand behind.
//
// Deliberately not "the network is up": a firmware installed at a desk and
// then carried to a room with a different Wi-Fi would roll itself back, and
// a router rebooting overnight would be read as a broken image. What is
// checked is what the update could plausibly have broken and what the device
// cannot work without - the two tasks, still turning.
void App::serviceRecovery(uint32_t nowMs) {
    DisplayTask::Stats stats;
    display_.snapshot(stats);
    const bool healthy = display_.running() && stats.fpsX10 > 0 && audio_.running();

    appdiag::Recovery::update(nowMs, healthy);

    // proven(), not confirmed(). An image flashed over USB is confirmed before
    // setup() returns - it was never on probation - so keying the backup off
    // that meant it only ever ran for images installed by OTA. Which is
    // backwards: the copy most worth having on the card is the one someone
    // deliberately flashed.
    if (!appdiag::Recovery::proven()) return;

    // The order here is the whole point. The image proves itself, then it is
    // put through the one job most likely to break it, then - having survived
    // that and a little more besides - the rollback is given up. Doing it the
    // other way round is how boot #4 of this device ended up with no way back:
    // it declared itself good and then died copying itself to the card.
    if (backupDone_) {
        if (appdiag::Recovery::confirmed()) return;
        if (static_cast<int32_t>(nowMs - settleUntilMs_) < 0) return;
        appdiag::Recovery::confirm();
        return;
    }
    if (!backupDueMs_) {
        // The recovery application counts its own attempts so it cannot spend
        // the afternoon reinstalling an image that does not help. A firmware
        // that has just proved itself is the evidence that clears that count.
        Preferences recov;
        if (recov.begin("m5recov", false)) {
            if (recov.getUInt("tries", 0)) recov.putUInt("tries", 0);
            recov.end();
        }
        // Let the moment pass before touching the card: this is a megabyte and
        // a half of flash reads and card writes on the loop task, and doing it
        // in the same tick as everything else settling is asking for a stutter
        // nobody needs to see.
        backupDueMs_ = nowMs + 3000;
        return;
    }
    if (static_cast<int32_t>(nowMs - backupDueMs_) < 0) return;
    backupDone_ = true;
    // Failure here is not worth retrying in a loop; the card may be absent,
    // and a companion with no card still works. The console reports it. Either
    // way the settle timer starts, because an image is not rolled back for
    // having nowhere to put a backup.
    FirmwareStore::backupRunning(&display_);
    settleUntilMs_ = millis() + appcfg::kSettleAfterBackupMs;
}

// The ladder out of a boot loop, climbed one rung per failed boot.
//
// Order is by how much it costs to be wrong. Trying the other OTA slot is
// free: if it holds the firmware this one replaced, that firmware used to
// work. Safe mode costs the character and the voice but keeps the device
// reachable, which is what makes it possible to look. The recovery
// application is last because it writes flash, and writing flash to fix a
// device that is already failing to boot is the move you want to have tried
// everything else before.
//
// Each rung is taken once. The flags live in RTC memory beside the counter,
// so pulling the power clears the whole ladder and starts again - which is
// the right meaning for that gesture.
void App::escalateRecovery() {
    const uint8_t streak = appdiag::Recovery::bootStreak();

    if (streak >= appcfg::kRecoveryAppStreak && appdiag::Recovery::factoryPresent() &&
        !appdiag::Recovery::triedFactory()) {
        appdiag::Recovery::noteTriedFactory();
        Serial.println("[recovery] handing over to the recovery application");
        appdiag::Recovery::bootFactory();   // does not return when it works
    }
    if (appdiag::Recovery::otherSlotBootable() && !appdiag::Recovery::triedOtherSlot()) {
        appdiag::Recovery::noteTriedOtherSlot();
        Serial.printf("[recovery] %u failed boots; trying %s\n", (unsigned)streak,
                      appdiag::Recovery::otherLabel());
        appdiag::Recovery::bootOtherSlot();  // does not return when it works
    }
}

// Drawn straight to the panel rather than through the display task, because
// the display task is exactly the thing not being started.
void App::showSafeModeScreen() {
    M5.Display.setBrightness(160);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(16, 24);
    M5.Display.print("SAFE MODE");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(16, 62);
    M5.Display.printf("%u boots in a row did not stay up.",
                      (unsigned)appdiag::Recovery::bootStreak());
    M5.Display.setCursor(16, 78);
    M5.Display.print("No character pack, no audio.");
    M5.Display.setCursor(16, 102);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.print("Connecting to Wi-Fi for the console...");
}

void App::updateSafeModeScreen() {
    // Only the address changes, and only once.
    if (safeModeAddressShown_ || !network_.wifiConnected()) return;
    safeModeAddressShown_ = true;
    M5.Display.fillRect(0, 96, appcfg::kScreenW, 60, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.setCursor(16, 102);
    M5.Display.print("Open this to put it right:");
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.setCursor(16, 122);
    M5.Display.printf("http://%s/", network_.ipAddress());
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(16, 156);
    M5.Display.print("Firmware tab: restore a backup from the card,");
    M5.Display.setCursor(16, 170);
    M5.Display.print("or install a new build. Then Boot normally.");
}

void App::factoryReset() {
    configStore_.clear();
    display_.showMessage("Reset", "settings cleared", "rebooting...");
    leds_.off();
    appdiag::BootLog::noteCleanShutdown("factory reset");
    delay(1200);
    ESP.restart();
}

void App::handleEvent(const AppEvent& event, uint32_t nowMs) {
    switch (event.type) {
        case AppEventType::PttPressed:
            if (state_.state() != CompanionState::Sleep) {
                audio_.requestCapture();
                network_.sendListenBegin();
                // The radio waits for the face to finish changing; see
                // kUplinkHoldoffMs. The microphone does not - the queue holds
                // what it captures in the meantime.
                uplinkOpensAtMs_ = nowMs +
                    (board::kNeedsUplinkHoldoff ? appcfg::kUplinkHoldoffMs : 0);
                // Said out loud, and therefore kept in the RTC tail, because
                // this is the moment the device is most likely to die on a
                // tired battery and the next boot needs to know that is where
                // it was.
                log_i("listen: begin (battery %u%%, %s, tx %d dBm)",
                      power_.batteryPercent(),
                      power_.charging() ? "on the cable" : "on battery",
                      network_.txPower());
            }
            director_.poke(nowMs);
            break;

        case AppEventType::PttReleased:
            if (audio_.capturing()) {
                audio_.requestIdle();
                network_.sendListenEnd();
                log_i("listen: end (%u chunks up, %u failed)",
                      (unsigned)network_.uplinkChunks(),
                      (unsigned)network_.uplinkFailures());
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
    // Still inside the hold-off. The chunks stay in the queue; they are not
    // dropped, only late.
    if (uplinkOpensAtMs_ && static_cast<int32_t>(millis() - uplinkOpensAtMs_) < 0) return;
    uplinkOpensAtMs_ = 0;
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

void App::powerTestThunk(uint32_t seconds, void* ctx) {
    if (ctx) static_cast<App*>(ctx)->console_.runPowerTest(seconds);
}

void App::statusThunk(String& out, void* ctx) {
    if (ctx) static_cast<App*>(ctx)->appendStatus(out);
}

// Everything the device can say about itself, in one object.
//
// One request rather than a dozen because the point is a snapshot: two numbers
// read a second apart from a device that is falling over do not describe the
// same device. The names are the ones the firmware uses internally, not
// prettier ones, so a value here can be grepped for in the source.
void App::appendStatus(String& out) {
    DisplayTask::Stats stats;
    display_.snapshot(stats);

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\",\"uptime_ms\":%u,"
             "\"boot\":%u,\"reason\":\"%s\","
             "\"state\":\"%s\",\"expression\":\"%s\","
             "\"wifi\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"server\":%s,"
             "\"battery\":%u,\"charging\":%s,\"boost_held\":%s,"
             "\"heap\":%u,\"min_heap\":%u,\"fps\":%u.%u,"
             "\"uplink_chunks\":%u,\"uplink_failures\":%u,"
             "\"spk_dropped\":%u,\"mic_dropped\":%u,"
             "\"volume\":%u,\"muted\":%s,\"brightness\":%u,\"pack\":\"%s\","
             "\"sd_bytes_per_sec\":%u,\"ota\":%s,\"log_seq\":%u,"
             "\"slot\":\"%s\",\"safe_mode\":%s,\"on_trial\":%s,\"confirmed\":%s,"
             "\"boot_streak\":%u,\"proven\":%s,\"backed_up\":%s,"
             "\"battery_saver\":%s,\"tx_power_dbm\":%d}",
             config_.deviceName.c_str(), M5COMPANION_VERSION, network_.ipAddress(),
             (unsigned)millis(), (unsigned)appdiag::BootLog::bootCount(),
             appdiag::resetReasonName(), stateName(state_.state()),
             expressionName(state_.expression()),
             network_.wifiConnected() ? "true" : "false", config_.wifiSsid.c_str(),
             (int)network_.rssi(), network_.serverConnected() ? "true" : "false",
             power_.batteryPercent(), power_.charging() ? "true" : "false",
             power_.boostHeld() ? "true" : "false", (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMinFreeHeap(), stats.fpsX10 / 10, stats.fpsX10 % 10,
             (unsigned)network_.uplinkChunks(), (unsigned)network_.uplinkFailures(),
             audio_.droppedPlayback(), audio_.droppedCapture(), config_.volume,
             muted_ ? "true" : "false", power_.activeBrightness(), config_.packName.c_str(),
             (unsigned)stats.sdBytesPerSec, ota_.running() ? "true" : "false",
             (unsigned)appdiag::LogRing::seq(), appdiag::Recovery::runningLabel(),
             safeMode_ ? "true" : "false", appdiag::Recovery::onTrial() ? "true" : "false",
             appdiag::Recovery::confirmed() ? "true" : "false",
             (unsigned)appdiag::Recovery::bootStreak(),
             appdiag::Recovery::proven() ? "true" : "false",
             backupDone_ ? "true" : "false",
             batterySaver_ ? "true" : "false", network_.txPower());
    out += buf;
}

uint8_t App::volumeIndex(uint8_t volume) {
    for (uint8_t i = 0; i < appcfg::kVolumeStepCount; ++i) {
        if (appcfg::kVolumeSteps[i] >= volume) return i;
    }
    return appcfg::kVolumeStepCount - 1;
}

void App::serviceButtons(uint32_t nowMs) {
    if (input_.factoryResetRequested()) factoryReset();

    if (input_.consumeMuteToggle()) {
        muted_ = !muted_;
        audio_.setMuted(muted_);
        state_.noteInteraction(nowMs);
        director_.poke(nowMs);
    }
    if (input_.consumeVolumeStep()) {
        // Up one step, and round again from the bottom. Wrapping rather than
        // stopping at the top is what lets one button do the whole range.
        uint8_t next = 0;
        for (uint8_t i = 0; i < appcfg::kVolumeStepCount; ++i) {
            if (appcfg::kVolumeSteps[i] > config_.volume) {
                next = appcfg::kVolumeSteps[i];
                break;
            }
        }
        if (!next) next = appcfg::kVolumeSteps[0];
        config_.volume = next;
        configStore_.save(config_);
        audio_.setVolume(next);
        // A change nobody can hear until the next reply is a change nobody can
        // judge, so say it on the screen.
        director_.setVolume(volumeIndex(next), appcfg::kVolumeStepCount);
        state_.noteInteraction(nowMs);
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
    // Nobody was watching this task. The Arduino port subscribes core 0's idle
    // task to the watchdog and not core 1's, so a loop that stopped turning
    // stopped everything and reported nothing - which is what "it freezes on
    // LISTEN with the cable out" looked like: no reboot, no log, no way in.
    // Watched, the same hang becomes a reboot that names itself in the next
    // hello, and the device comes back rather than sitting there.
    if (!loopWatched_) {
        // Five seconds is the default, and five seconds is also exactly how
        // long one legitimate operation on this task can take: WebServer sets
        // its client's timeout to HTTP_MAX_SEND_WAIT before writing a response
        // (WebServer.cpp:305), so a browser tab closed mid-reply - or a phone
        // that walks out of range - blocks the write for the full five and
        // then the watchdog fires on a task that was never stuck.
        //
        // A threshold set at the maximum of what it is meant to tolerate is
        // not a threshold. Ten gives the bounded stall room to finish while
        // still turning a real hang into a reboot, which is the whole reason
        // this task is watched at all.
        esp_task_wdt_init(appcfg::kLoopWatchdogSeconds, true);
        loopWatched_ = esp_task_wdt_add(nullptr) == ESP_OK;
        if (loopWatched_) {
            log_i("loop task watched, %u s", (unsigned)appcfg::kLoopWatchdogSeconds);
        }
    }
    if (loopWatched_) esp_task_wdt_reset();

    // A file transfer owns the SPI bus and the serial port; everything else
    // waits rather than fighting it.
    console_.poll();
    if (console_.busy()) return;

    M5.update();
    const uint32_t now = millis();

    network_.loop(now);

    // The console only exists once there is a network to reach it on.
    if (network_.wifiConnected() && config_.webServerEnabled) {
        if (!web_.running()) {
            web_.begin(&display_, &configStore_, &config_, &ota_,
                       config_.otaPassword.c_str());
            web_.setStatusProvider(statusThunk, this);
            web_.setPowerTest(powerTestThunk, this);
            ota_.begin(config_.deviceName.c_str(), config_.otaPassword.c_str(), &display_,
                       &audio_);
        }
        ota_.loop();
        web_.loop();
        // An upload owns the card, and an update owns the flash; either way
        // nothing else in this loop should run until it is finished.
        if (web_.busy() || ota_.busy() || web_.rebootPending()) return;
    }
    // Safe mode stops here: the network and the console are up, and nothing
    // else was started to service.
    if (safeMode_) {
        updateSafeModeScreen();
        appdiag::BootLog::heartbeat(now, power_.batteryPercent(), power_.charging());
        power_.update(now, false);
        // Safe mode still runs on the same battery, and the radio is the one
        // load it has not given up - capping it here too costs nothing and is
        // the difference between a reachable device and another lock-up.
        applyPowerPolicy(false);
        delay(5);
        return;
    }

    input_.update(events_, now);
    serviceButtons(now);

    AppEvent event;
    while (events_.poll(event)) handleEvent(event, now);

    // The state machine and the microphone can disagree - a release that was
    // missed, a timeout that fired - and whichever is not listening wins,
    // because a microphone running with nothing to send it is the failure that
    // fills the queue and looks like a frozen device.
    if (audio_.capturing() && state_.state() != CompanionState::Listening) {
        audio_.requestIdle();
        network_.sendListenEnd(true);
    }

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
    director_.setTouchAction(input_.activeTouchAction());
    director_.setDeviceStatus(power_.batteryPercent(), power_.charging(), network_.wifiConnected(),
                              network_.serverConnected(), muted_);
    display_.submit(director_.update(now));

    leds_.update(state_.state(), audio_.lipLevel(), network_.serverConnected(), muted_, now);
    power_.update(now, state_.state() == CompanionState::Sleep);
    applyPowerPolicy(false);

    // The line the next boot reads back if this one ends without warning.
    appdiag::BootLog::heartbeat(now, power_.batteryPercent(), power_.charging());
    serviceRecovery(now);

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
