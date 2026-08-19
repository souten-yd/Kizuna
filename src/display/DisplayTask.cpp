#include "DisplayTask.hpp"

#include <M5Unified.h>
#include <SD.h>

#include "AppConfig.hpp"

namespace {

constexpr uint16_t kAccentBlue = 0x4E7F;
constexpr uint16_t kAccentRed = 0xF9A6;

// Finds a pack directory under /companion/packs. `preferred` wins when it
// exists, otherwise the first directory containing a manifest is used.
bool resolvePackDir(const char* preferred, String& out) {
    const String packsRoot = String(appcfg::kAssetRoot) + "/packs";

    if (preferred && *preferred) {
        const String candidate = packsRoot + "/" + preferred;
        if (SD.exists((candidate + "/manifest.json").c_str())) {
            out = candidate;
            return true;
        }
    }

    File dir = SD.open(packsRoot.c_str());
    if (!dir || !dir.isDirectory()) return false;
    for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        if (entry.isDirectory()) {
            const String candidate = packsRoot + "/" + String(entry.name()).substring(
                                                          String(entry.name()).lastIndexOf('/') + 1);
            if (SD.exists((candidate + "/manifest.json").c_str())) {
                out = candidate;
                entry.close();
                dir.close();
                return true;
            }
        }
        entry.close();
    }
    dir.close();
    return false;
}

}  // namespace

void DisplayTask::setPackName(const char* name) {
    if (name && *name) strlcpy(packName_, name, sizeof(packName_));
}

bool DisplayTask::begin() {
    frameQueue_ = xQueueCreate(1, sizeof(FaceFrame));
    commandQueue_ = xQueueCreate(6, sizeof(CommandMsg));
    if (!frameQueue_ || !commandQueue_) return false;
    return xTaskCreatePinnedToCore(taskThunk, "display", 8192, this, 3, &task_, 1) == pdPASS;
}

void DisplayTask::submit(const FaceFrame& frame) {
    if (frameQueue_) xQueueOverwrite(frameQueue_, &frame);
}

void DisplayTask::post(const CommandMsg& msg) {
    if (commandQueue_) xQueueSend(commandQueue_, &msg, 0);
}

void DisplayTask::showMessage(const char* title, const char* line1, const char* line2) {
    CommandMsg msg;
    msg.cmd = Command::ShowMessage;
    strlcpy(msg.title, title ? title : "", sizeof(msg.title));
    strlcpy(msg.line1, line1 ? line1 : "", sizeof(msg.line1));
    strlcpy(msg.line2, line2 ? line2 : "", sizeof(msg.line2));
    post(msg);
}

bool DisplayTask::pause(uint32_t timeoutMs) {
    pauseRequested_ = true;
    const uint32_t deadline = millis() + timeoutMs;
    while (!paused_ && static_cast<int32_t>(deadline - millis()) > 0) delay(5);
    return paused_;
}

void DisplayTask::resume() {
    pauseRequested_ = false;
    CommandMsg msg;
    msg.cmd = Command::Repaint;
    post(msg);
}

void DisplayTask::taskThunk(void* ctx) {
    static_cast<DisplayTask*>(ctx)->run();
}

void DisplayTask::snapshot(Stats& out) const {
    portENTER_CRITICAL(&statsMux_);
    out = stats_;
    portEXIT_CRITICAL(&statsMux_);
}

uint8_t DisplayTask::eyeSlotFor(const FaceFrame& f) const {
    switch (f.eye) {
        case EyeFrame::SoftLower:    return m5a::kEyeSoftLower;
        case EyeFrame::Half:         return m5a::kEyeHalf;
        case EyeFrame::AlmostClosed: return m5a::kEyeAlmostClosed;
        case EyeFrame::Closed:       return m5a::kEyeClosed;
        case EyeFrame::Wide:         return m5a::kEyeWide;
        case EyeFrame::SleepyHalf:   return m5a::kEyeSleepyHalf;
        case EyeFrame::SleepyClosed: return m5a::kEyeSleepyClosed;
        default: break;
    }
    // Only a fully open eye carries a gaze direction; a half-shut lid hides
    // enough of the iris that the direction would not read anyway.
    if (f.gazeX <= -2) return m5a::kEyeOpenLeft;
    if (f.gazeX >= 2) return m5a::kEyeOpenRight;
    if (f.gazeY <= -2) return m5a::kEyeOpenUp;
    if (f.gazeY >= 2) return m5a::kEyeOpenDown;
    return m5a::kEyeOpenCenter;
}

void DisplayTask::bootSequence() {
    renderer_.begin(nullptr, nullptr);
    fallback_.begin();
    renderer_.drawMessage("M5Companion", M5COMPANION_VERSION, "starting up...", kAccentBlue);
    renderer_.drawBootProgress("mounting microSD", 10);

    if (!sd_.mount()) {
        strlcpy(stats_.sdStatus, sd_.statusText(), sizeof(stats_.sdStatus));
        renderer_.drawMessage("No assets", sd_.statusText(), sd_.hint(), kAccentRed);
        delay(2200);
        fallback_.invalidate();
        return;
    }
    strlcpy(stats_.sdStatus, sd_.statusText(), sizeof(stats_.sdStatus));
    stats_.sdMounted = true;

    renderer_.drawBootProgress("loading character pack", 35);
    String packDir;
    if (!resolvePackDir(packName_, packDir) || !pack_.load(packDir.c_str())) {
        renderer_.drawMessage("No character pack", pack_.error(),
                              "run tools/pack_assets.py", kAccentRed);
        delay(2200);
        fallback_.invalidate();
        return;
    }
    strlcpy(stats_.packName, pack_.name(), sizeof(stats_.packName));

    renderer_.drawBootProgress("measuring card speed", 60);
    const uint32_t bps = sd_.measure(pack_.benchmarkPath());
    budget_.configure(bps);
    log_i("frame budget: %u B/tick (~%u B/s)", budget_.bytesPerTick(),
          budget_.estimatedBytesPerSec());

    renderer_.drawBootProgress("preparing cache", 80);
    const size_t entryBytes = max(pack_.layerBytes(AssetPack::Layer::Eyes),
                                  pack_.layerBytes(AssetPack::Layer::Mouth));
    cache_.begin(entryBytes, appcfg::kTileCacheBytes, appcfg::kTileCacheMinBytes);
    renderer_.begin(&pack_, &cache_);

    renderer_.drawBootProgress("ready", 100);
    assetsReady_ = true;

    // Optional cinematic intro. Frames come straight off the card, so a long
    // boot clip costs storage rather than RAM.
    const uint16_t bootFrames = pack_.clipFrameCount("boot");
    if (bootFrames) {
        uint16_t fps = pack_.clipFps("boot");
        if (!fps) fps = 5;
        fps = min<uint16_t>(fps, appcfg::kGestureMaxFps);
        const uint32_t frameMs = 1000 / fps;
        for (uint16_t i = 0; i < bootFrames; ++i) {
            const uint32_t t0 = millis();
            renderer_.drawClipFrame("boot", i);
            const uint32_t spent = millis() - t0;
            if (spent < frameMs) delay(frameMs - spent);
        }
    }

    renderer_.requestBase(Expression::Neutral);
    drawnExpression_ = Expression::Neutral;
}

bool DisplayTask::playGesture(Gesture gesture) {
    const char* name = gestureName(gesture);
    if (!name || !*name || !pack_.hasClip(name)) return false;

    const uint16_t frames = pack_.clipFrameCount(name);
    if (!frames) return false;

    uint16_t fps = pack_.clipFps(name);
    if (!fps) fps = appcfg::kGestureMaxFps;
    fps = max<uint16_t>(1, min<uint16_t>(fps, appcfg::kGestureMaxFps));
    const uint32_t frameMs = 1000UL / fps;

    // Gesture frames are capped because SD and LCD share one SPI bus. The cap
    // is what the bus can actually carry, not a fixed number: clips now send
    // only the tiles that moved, so the same bus buys roughly 9 fps instead of
    // the 5 a whole 320x240 RGB565 frame allowed.
    const CompanionState startState = current_.state;
    for (uint16_t i = 0; i < frames; ++i) {
        if (pauseRequested_ || uxQueueMessagesWaiting(commandQueue_)) break;

        // Do not let a comic idle clip hide a real interaction. Frames arrive
        // through a one-deep mailbox, so sampling it here gives PTT/speech and
        // wake/sleep transitions a bounded interruption latency (one gesture
        // frame, at most 200 ms with the 5 fps cap).
        FaceFrame incoming;
        if (xQueueReceive(frameQueue_, &incoming, 0) == pdTRUE) {
            current_ = incoming;
            if (current_.state != startState) break;
            if (current_.gestureToken != drawnGestureToken_) {
                drawnGestureToken_ = current_.gestureToken;
                break;
            }
        }

        const uint32_t t0 = millis();
        bytesWindow_ += renderer_.drawClipFrame(name, i);
        const uint32_t spent = millis() - t0;
        if (spent < frameMs) vTaskDelay(pdMS_TO_TICKS(frameMs - spent));
    }

    // A clip covers the whole screen. Force the normal layered renderer to
    // rebuild its baseline and tiles before it resumes.
    drawnExpression_ = Expression::Count;
    drawnSway_ = 0xFF;
    drawnEyeSlot_ = 0xFF;
    drawnViseme_ = 0xFF;
    renderer_.requestBase(current_.expression);
    renderer_.markOverlayDirty();
    return true;
}

void DisplayTask::handleCommands() {
    CommandMsg msg;
    while (xQueueReceive(commandQueue_, &msg, 0) == pdTRUE) {
        switch (msg.cmd) {
            case Command::ReloadPack:
                cache_.invalidateAll();
                pack_.unload();
                assetsReady_ = false;
                bootSequence();
                break;
            case Command::ToggleDebug:
                debug_ = !debug_;
                renderer_.markOverlayDirty();
                break;
            case Command::SetDebug:
                debug_ = msg.value != 0;
                renderer_.markOverlayDirty();
                break;
            case Command::ShowMessage:
                renderer_.drawMessage(msg.title, msg.line1, msg.line2, kAccentRed);
                messageMode_ = true;
                break;
            case Command::Repaint:
                messageMode_ = false;
                drawnExpression_ = Expression::Count;
                drawnSway_ = 0xFF;
                drawnEyeSlot_ = 0xFF;
                drawnViseme_ = 0xFF;
                fallback_.invalidate();
                renderer_.markOverlayDirty();
                break;
            default:
                break;
        }
    }
}

void DisplayTask::renderProcedural(uint32_t nowMs) {
    FaceFrame f = current_;
    f.showDebug = debug_;
    fallback_.render(f, !sd_.mounted(), sd_.hint());
    if (renderer_.overlayDirty() || nowMs - lastOverlayMs_ >= 1000) {
        renderer_.drawOverlay(f, budget_, stats_.fpsX10);
        lastOverlayMs_ = nowMs;
    }
}

void DisplayTask::renderTick(uint32_t nowMs) {
    budget_.beginTick();
    size_t moved = 0;

    // Gesture requests are edge-triggered by token, not by enum value. This
    // allows e.g. two nods in a row without a dummy "none" frame between them.
    if (current_.gestureToken != drawnGestureToken_) {
        drawnGestureToken_ = current_.gestureToken;
        if (current_.gesture != Gesture::None && playGesture(current_.gesture)) return;
    }

    if (current_.expression != drawnExpression_ && !renderer_.basePending()) {
        renderer_.requestBase(current_.expression);
        drawnExpression_ = current_.expression;
        drawnSway_ = 0xFF;
        drawnEyeSlot_ = 0xFF;
        drawnViseme_ = 0xFF;
    }

    if (renderer_.basePending()) {
        moved += renderer_.stepBase(budget_);
    } else {
        const Expression e = current_.expression;
        const uint8_t slot = eyeSlotFor(current_);
        const uint8_t sway = current_.swayFrame % pack_.swayFrames();

        const size_t swayBytes = pack_.layerBytes(AssetPack::Layer::Sway);
        const size_t eyeBytes = pack_.layerBytes(AssetPack::Layer::Eyes);
        const size_t mouthBytes = pack_.layerBytes(AssetPack::Layer::Mouth);

        if (sway != drawnSway_) {
            // A body frame repaints the area underneath the eyes and mouth, so
            // the three layers advance as one indivisible unit. Deferring the
            // whole bundle is what keeps lip sync from tearing.
            const size_t bundle = swayBytes + eyeBytes + mouthBytes;
            if (budget_.take(bundle)) {
                moved += renderer_.drawTile(e, AssetPack::Layer::Sway, pack_.swayIndex(sway));
                moved += renderer_.drawTile(e, AssetPack::Layer::Eyes, pack_.eyeIndex(sway, slot));
                moved += renderer_.drawTile(e, AssetPack::Layer::Mouth,
                                            pack_.mouthIndex(sway, current_.viseme));
                drawnSway_ = sway;
                drawnEyeSlot_ = slot;
                drawnViseme_ = current_.viseme;
            }
        }

        // Between body frames the mouth and eyes may still move on their own.
        if (drawnSway_ == sway) {
            if (current_.viseme != drawnViseme_ && budget_.take(mouthBytes)) {
                moved += renderer_.drawTile(e, AssetPack::Layer::Mouth,
                                            pack_.mouthIndex(sway, current_.viseme));
                drawnViseme_ = current_.viseme;
            }
            if (slot != drawnEyeSlot_ && budget_.take(eyeBytes)) {
                moved += renderer_.drawTile(e, AssetPack::Layer::Eyes, pack_.eyeIndex(sway, slot));
                drawnEyeSlot_ = slot;
            }
        }
    }

    if (renderer_.overlayDirty() || nowMs - lastOverlayMs_ >= 1000) {
        FaceFrame f = current_;
        f.showDebug = debug_;
        renderer_.drawOverlay(f, budget_, stats_.fpsX10);
        lastOverlayMs_ = nowMs;
    }

    bytesWindow_ += moved;
}

void DisplayTask::run() {
    bootSequence();

    uint32_t nextTick = millis();
    for (;;) {
        if (pauseRequested_) {
            paused_ = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            nextTick = millis();
            continue;
        }
        paused_ = false;

        handleCommands();

        FaceFrame incoming;
        if (xQueueReceive(frameQueue_, &incoming, 0) == pdTRUE) current_ = incoming;

        const uint32_t now = millis();
        if (!messageMode_) {
            if (assetsReady_ && pack_.ready()) {
                renderTick(now);
            } else {
                renderProcedural(now);
            }
        }

        ++frameCounter_;
        if (now - fpsWindowMs_ >= 1000) {
            const uint32_t span = now - fpsWindowMs_;
            portENTER_CRITICAL(&statsMux_);
            stats_.fpsX10 = span ? (frameCounter_ * 10000) / span : 0;
            stats_.drawnBytesPerSec = span ? (bytesWindow_ * 1000) / span : 0;
            stats_.sdBytesPerSec = sd_.readBytesPerSec();
            stats_.budgetBytesPerSec = budget_.estimatedBytesPerSec();
            stats_.cacheHits = cache_.hits();
            stats_.cacheMisses = cache_.misses();
            stats_.cacheSlots = cache_.slots();
            stats_.packReady = pack_.ready();
            stats_.sdMounted = sd_.mounted();
            portEXIT_CRITICAL(&statsMux_);
            frameCounter_ = 0;
            bytesWindow_ = 0;
            fpsWindowMs_ = now;
        }

        nextTick += appcfg::kDisplayTickMs;
        const int32_t sleep = static_cast<int32_t>(nextTick - millis());
        if (sleep > 0) {
            vTaskDelay(pdMS_TO_TICKS(sleep));
        } else {
            // The card fell behind; resynchronise instead of spinning.
            nextTick = millis();
            vTaskDelay(1);
        }
    }
}
