#pragma once

#include "EventBus.hpp"
#include "SerialConsole.hpp"
#include "StateMachine.hpp"
#include "audio/AudioManager.hpp"
#include "character/CharacterDirector.hpp"
#include "device/LedController.hpp"
#include "device/PowerManager.hpp"
#include "display/DisplayTask.hpp"
#include "input/InputController.hpp"
#include "network/NetworkManager.hpp"
#include "network/DeviceWebServer.hpp"
#include "network/OtaService.hpp"
#include "storage/ConfigStore.hpp"

class App {
public:
    void begin();
    void loop();

private:
    static void binarySinkThunk(const uint8_t* data, size_t len, void* ctx);
    void handleEvent(const AppEvent& event, uint32_t nowMs);
    void serviceAudioUplink();
    void serviceButtons(uint32_t nowMs);
    static uint8_t volumeIndex(uint8_t volume);
    void serviceAmbient(uint32_t nowMs);
    static void statusThunk(String& out, void* ctx);
    static void powerTestThunk(uint32_t seconds, void* ctx);
    void appendStatus(String& out);
    void serviceRecovery(uint32_t nowMs);
    void applyPowerPolicy(bool force);
    void escalateRecovery();
    void showSafeModeScreen();
    void updateSafeModeScreen();
    void maybeProvision();
    void factoryReset();

    EventBus events_;
    StateMachine state_;
    CharacterDirector director_;
    DisplayTask display_;
    AudioManager audio_;
    NetworkManager network_;
    DeviceWebServer web_;
    OtaService ota_;
    InputController input_;
    LedController leds_;
    PowerManager power_;
    ConfigStore configStore_;
    SerialConsole console_;
    DeviceConfig config_;

    bool muted_ = false;
    bool debug_ = false;
    bool loopWatched_ = false;
    bool safeMode_ = false;
    bool safeModeAddressShown_ = false;
    bool backupDone_ = false;
    uint32_t backupDueMs_ = 0;
    uint32_t settleUntilMs_ = 0;
    bool batterySaver_ = false;
    bool powerPolicyKnown_ = false;
    uint32_t uplinkOpensAtMs_ = 0;
    bool speechEndPending_ = false;
    uint32_t nextAmbientMs_ = 0;
    uint32_t lastTelemetryMs_ = 0;
    uint32_t lastStatePushMs_ = 0;
    CompanionState lastReportedState_ = CompanionState::Count;
};
