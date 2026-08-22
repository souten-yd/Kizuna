#include "InputController.hpp"

#include <M5Unified.h>
#include <math.h>

#include "AppConfig.hpp"
#include "Board.hpp"

void InputController::begin(uint32_t nowMs) {
    lastMotionMs_ = nowMs;
}

bool InputController::consumeMuteToggle() {
    const bool v = muteToggle_;
    muteToggle_ = false;
    return v;
}

bool InputController::consumeDebugToggle() {
    const bool v = debugToggle_;
    debugToggle_ = false;
    return v;
}

bool InputController::consumeBrightnessStep() {
    const bool v = brightnessStep_;
    brightnessStep_ = false;
    return v;
}

bool InputController::consumeVolumeStep() {
    const bool v = volumeStep_;
    volumeStep_ = false;
    return v;
}

bool InputController::consumeSleepToggle() {
    const bool v = sleepToggle_;
    sleepToggle_ = false;
    return v;
}

void InputController::update(EventBus& events, uint32_t nowMs) {
    if constexpr (board::kHasTouch) {
        updateCoreS3Touch(events, nowMs);
        updateMotion(events, nowMs);
        return;
    }

    // A is push-to-talk: hold to speak, release to send.
    if (M5.BtnA.wasPressed()) events.post(AppEvent(AppEventType::PttPressed));
    if (M5.BtnA.wasReleased()) events.post(AppEvent(AppEventType::PttReleased));

    // A tap steps the volume, a hold silences it. The two things a person
    // reaches for mid-sentence are "louder" and "stop", and they should not be
    // the same gesture.
    if (M5.BtnB.wasClicked()) volumeStep_ = true;
    if (M5.BtnB.wasHold()) muteToggle_ = true;
    if (M5.BtnC.wasClicked()) brightnessStep_ = true;
    if (M5.BtnC.wasHold()) sleepToggle_ = true;

    // B+C is the only destructive gesture, so it needs a long, deliberate hold.
    if (M5.BtnB.isPressed() && M5.BtnC.isPressed()) {
        if (!bcHeldSinceMs_) bcHeldSinceMs_ = nowMs;
        if (nowMs - bcHeldSinceMs_ > appcfg::kFactoryResetHoldMs) factoryReset_ = true;
    } else {
        bcHeldSinceMs_ = 0;
    }

    updateMotion(events, nowMs);
}

void InputController::updateCoreS3Touch(EventBus& events, uint32_t nowMs) {
    const auto& touch = M5.Touch.getDetail();

    if (touch.wasPressed()) {
        touchAction_ = touchui::actionAt(touch.x, touch.y);
        touchStartedMs_ = nowMs;
        touchLongActionFired_ = false;
        if (touchAction_ == touchui::Action::Talk) {
            events.post(AppEvent(AppEventType::PttPressed));
        }
    }

    if (touch.isPressed() && touchAction_ != touchui::Action::None &&
        touchui::actionAt(touch.x, touch.y) == touchAction_) {
        const uint32_t heldMs = nowMs - touchStartedMs_;
        if (!touchLongActionFired_ && heldMs >= touchui::kLongPressMs) {
            if (touchAction_ == touchui::Action::Volume) {
                muteToggle_ = true;
                touchLongActionFired_ = true;
            } else if (touchAction_ == touchui::Action::Brightness) {
                sleepToggle_ = true;
                touchLongActionFired_ = true;
            }
        }
        if (touchAction_ == touchui::Action::Settings &&
            heldMs >= appcfg::kFactoryResetHoldMs) {
            factoryReset_ = true;
            touchLongActionFired_ = true;
        }
    }

    if (!touch.wasReleased()) return;

    const touchui::Action released = touchui::actionAt(touch.x, touch.y);
    const touchui::Action action = touchAction_;
    const bool longActionFired = touchLongActionFired_;
    touchAction_ = touchui::Action::None;
    touchStartedMs_ = 0;
    touchLongActionFired_ = false;

    if (action == touchui::Action::Talk) {
        events.post(AppEvent(AppEventType::PttReleased));
        return;
    }
    // Sliding away cancels taps, which keeps a missed icon from firing its
    // neighbour when the finger is lifted.
    if (action == touchui::Action::None || action != released || longActionFired) return;

    switch (action) {
        case touchui::Action::Volume:     volumeStep_ = true; break;
        case touchui::Action::Brightness: brightnessStep_ = true; break;
        default: break;
    }
}

void InputController::updateMotion(EventBus& events, uint32_t nowMs) {
    if (!M5.Imu.update()) return;

    const auto d = M5.Imu.getImuData();

    // Low-pass the tilt so the gaze glides instead of twitching.
    filtX_ += (d.accel.x - filtX_) * 0.18f;
    filtY_ += (d.accel.y - filtY_) * 0.18f;
    gazeX_ = static_cast<int8_t>(constrain(static_cast<int>(filtX_ * 9.0f), -4, 4));
    gazeY_ = static_cast<int8_t>(constrain(static_cast<int>(-filtY_ * 7.0f), -3, 3));

    const float mag =
        sqrtf(d.accel.x * d.accel.x + d.accel.y * d.accel.y + d.accel.z * d.accel.z);
    const float delta = fabsf(mag - 1.0f);

    if (!restKnown_) {
        restZ_ = d.accel.z;
        restKnown_ = true;
    }

    if (delta > 0.75f && nowMs - lastShakeMs_ > appcfg::kShakeCooldownMs) {
        lastShakeMs_ = nowMs;
        lastMotionMs_ = nowMs;
        events.post(AppEvent(AppEventType::MotionShake));
        return;
    }

    // A lift shows up as a sustained, gentler deviation than a shake plus a
    // change in which way is down.
    if (delta > 0.18f && delta <= 0.75f && fabsf(d.accel.z - restZ_) > 0.20f &&
        nowMs - lastPickupMs_ > appcfg::kPickupCooldownMs) {
        lastPickupMs_ = nowMs;
        lastMotionMs_ = nowMs;
        restZ_ = d.accel.z;
        events.post(AppEvent(AppEventType::MotionPickup));
        return;
    }

    if (delta > 0.05f) {
        lastMotionMs_ = nowMs;
    } else if (nowMs - lastMotionMs_ > 60000) {
        lastMotionMs_ = nowMs;
        events.post(AppEvent(AppEventType::MotionStill));
    }
}
