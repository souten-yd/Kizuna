#pragma once

#include <Arduino.h>

#include "app/EventBus.hpp"
#include "TouchUi.hpp"

// Buttons and MPU6886 gestures.
//
// The IMU work matters more than it looks: tilt-driven gaze and a startle on
// pickup are what make the companion feel like it notices the room, and none
// of it needs the server to be reachable.
class InputController {
public:
    void begin(uint32_t nowMs);
    void update(EventBus& events, uint32_t nowMs);

    int8_t gazeX() const { return gazeX_; }
    int8_t gazeY() const { return gazeY_; }

    bool factoryResetRequested() const { return factoryReset_; }
    bool consumeMuteToggle();
    bool consumeDebugToggle();
    bool consumeBrightnessStep();
    bool consumeSleepToggle();
    bool consumeVolumeStep();
    uint8_t activeTouchAction() const { return static_cast<uint8_t>(touchAction_); }

private:
    void updateMotion(EventBus& events, uint32_t nowMs);
    void updateCoreS3Touch(EventBus& events, uint32_t nowMs);

    float filtX_ = 0.0f;
    float filtY_ = 0.0f;
    int8_t gazeX_ = 0;
    int8_t gazeY_ = 0;

    uint32_t bcHeldSinceMs_ = 0;
    uint32_t lastShakeMs_ = 0;
    uint32_t lastPickupMs_ = 0;
    uint32_t lastMotionMs_ = 0;
    float restZ_ = 1.0f;
    bool restKnown_ = false;

    bool factoryReset_ = false;
    bool muteToggle_ = false;
    bool debugToggle_ = false;
    bool brightnessStep_ = false;
    bool sleepToggle_ = false;
    bool volumeStep_ = false;
    touchui::Action touchAction_ = touchui::Action::None;
    uint32_t touchStartedMs_ = 0;
    bool touchLongActionFired_ = false;
};
