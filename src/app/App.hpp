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
#include "network/PackWebServer.hpp"
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
    void maybeProvision();
    void factoryReset();

    EventBus events_;
    StateMachine state_;
    CharacterDirector director_;
    DisplayTask display_;
    AudioManager audio_;
    NetworkManager network_;
    PackWebServer web_;
    InputController input_;
    LedController leds_;
    PowerManager power_;
    ConfigStore configStore_;
    SerialConsole console_;
    DeviceConfig config_;

    bool muted_ = false;
    bool debug_ = false;
    bool speechEndPending_ = false;
    uint32_t lastTelemetryMs_ = 0;
    uint32_t lastStatePushMs_ = 0;
    CompanionState lastReportedState_ = CompanionState::Count;
};
