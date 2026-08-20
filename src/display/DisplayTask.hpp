#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "AppTypes.hpp"
#include "AssetPack.hpp"
#include "FrameBudget.hpp"
#include "ProceduralFace.hpp"
#include "Renderer.hpp"
#include "TileCache.hpp"
#include "storage/SdCard.hpp"

// The single owner of the shared SPI bus (LCD + microSD).
//
// No other task may touch M5.Display or the SD library. Frames arrive through
// a length-1 mailbox so a slow card can never build a backlog of stale poses:
// the renderer always draws the newest intent.
class DisplayTask {
public:
    enum class Command : uint8_t {
        None = 0,
        ReloadPack,
        ToggleDebug,
        SetDebug,
        ShowMessage,
        Repaint,
    };

    struct CommandMsg {
        Command cmd = Command::None;
        int32_t value = 0;
        char title[24] = {};
        char line1[40] = {};
        char line2[40] = {};
    };

    struct Stats {
        uint32_t fpsX10 = 0;
        uint32_t sdBytesPerSec = 0;
        uint32_t budgetBytesPerSec = 0;
        uint32_t drawnBytesPerSec = 0;
        uint32_t cacheHits = 0;
        uint32_t cacheMisses = 0;
        uint8_t cacheSlots = 0;
        bool packReady = false;
        bool sdMounted = false;
        char packName[24] = {};
        char sdStatus[32] = {};
    };

    // Must be called before begin(); the boot sequence reads it.
    void setPackName(const char* name);
    // Call before begin(): how much heap the tile cache must leave for what
    // starts later. Wi-Fi is not up yet when the cache is sized.
    void setHeapReserve(size_t bytes) { heapReserve_ = bytes; }
    bool begin();
    void submit(const FaceFrame& frame);
    void post(const CommandMsg& msg);
    void showMessage(const char* title, const char* line1, const char* line2);

    void snapshot(Stats& out) const;

    // Hands the SPI bus over to another task. The card and the panel share
    // MOSI/MISO/SCK, so anything that wants to write files must first get this
    // task to stop drawing - there is no locking scheme that makes concurrent
    // access safe. Returns false if the task did not stop in time.
    bool pause(uint32_t timeoutMs = 2000);
    void resume();
    bool assetsReady() const { return assetsReady_; }
    uint8_t swayFrames() const { return pack_.swayFrames(); }
    const char* sdHint() const { return sd_.hint(); }

private:
    size_t heapReserve_ = appcfg::kHeapReserveOffline;
    static void taskThunk(void* ctx);
    void run();
    void bootSequence();
    void handleCommands();
    void renderTick(uint32_t nowMs);
    void renderProcedural(uint32_t nowMs);
    bool playGesture(Gesture gesture);
    uint8_t eyeSlotFor(const FaceFrame& f) const;
    static uint8_t gazeStep(uint8_t from, uint8_t to);

    SdCard sd_;
    AssetPack pack_;
    TileCache cache_;
    Renderer renderer_;
    ProceduralFace fallback_;
    FrameBudget budget_;

    QueueHandle_t frameQueue_ = nullptr;    // length 1, overwrite semantics
    QueueHandle_t commandQueue_ = nullptr;
    TaskHandle_t task_ = nullptr;

    FaceFrame current_{};
    FaceFrame drawn_{};
    bool haveDrawn_ = false;
    bool assetsReady_ = false;
    bool messageMode_ = false;
    bool debug_ = false;
    volatile bool pauseRequested_ = false;
    volatile bool paused_ = false;
    char packName_[24] = "kizuna";

    uint8_t drawnSway_ = 0xFF;
    uint8_t drawnEyeSlot_ = 0xFF;
    uint8_t drawnViseme_ = 0xFF;
    uint16_t drawnGestureToken_ = 0;
    Expression drawnExpression_ = Expression::Count;

    uint32_t lastOverlayMs_ = 0;
    uint32_t frameCounter_ = 0;
    uint32_t fpsWindowMs_ = 0;
    uint32_t bytesWindow_ = 0;

    mutable portMUX_TYPE statsMux_ = portMUX_INITIALIZER_UNLOCKED;
    Stats stats_{};
};
