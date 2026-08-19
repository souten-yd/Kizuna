#include "InputController.hpp"

#include <M5Unified.h>
#include <math.h>

#include "AppConfig.hpp"

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

bool InputController::consumeSleepToggle() {
    const bool v = sleepToggle_;
    sleepToggle_ = false;
    return v;
}

void InputController::update(EventBus& events, uint32_t nowMs) {
    // A is push-to-talk: hold to speak, release to send.
    if (M5.BtnA.wasPressed()) events.post(AppEvent(AppEventType::PttPressed));
    if (M5.BtnA.wasReleased()) events.post(AppEvent(AppEventType::PttReleased));

    if (M5.BtnB.wasClicked()) muteToggle_ = true;
    if (M5.BtnB.wasHold()) debugToggle_ = true;
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
