#pragma once

#include <Arduino.h>

#include "AppTypes.hpp"

// Turns application state into a concrete pose, 30 times a second.
//
// Everything that makes the companion feel alive rather than merely correct
// lives here: blink timing, saccades, how fast the body breathes in each
// state, and how the mouth follows the audio envelope. The display task stays
// dumb on purpose - it only knows how to put rectangles on glass.
class CharacterDirector {
public:
    void begin(uint32_t nowMs);

    void setExpression(Expression e) { expression_ = e; }
    void setState(CompanionState s);
    void setTilt(int8_t gazeX, int8_t gazeY);
    void setLipLevel(uint8_t level);   // 0..kVisemeCount-1, from the audio task
    void setSwayFrameCount(uint8_t frames);
    void setDeviceStatus(uint8_t batteryPercent, bool charging, bool wifi, bool server, bool muted);
    void setDebug(bool on) { frame_.showDebug = on; }

    // Nudges the character: forces a look-at-you saccade and a quick blink,
    // used when the user presses a button or picks the device up.
    void poke(uint32_t nowMs);
    void startle(uint32_t nowMs);

    const FaceFrame& update(uint32_t nowMs);
    const FaceFrame& frame() const { return frame_; }

private:
    void updateBlink(uint32_t nowMs);
    void updateGaze(uint32_t nowMs);
    void updateSway(uint32_t nowMs);
    void updateViseme(uint32_t nowMs);
    uint8_t swayFpsForState() const;
    void scheduleBlink(uint32_t nowMs, bool soon);

    FaceFrame frame_{};
    Expression expression_ = Expression::Neutral;
    CompanionState state_ = CompanionState::Boot;

    // blink
    uint32_t nextBlinkMs_ = 0;
    uint32_t blinkStepMs_ = 0;
    uint8_t blinkStep_ = 0;       // 0 = not blinking
    uint8_t blinkRepeats_ = 0;

    // gaze
    int8_t tiltX_ = 0;
    int8_t tiltY_ = 0;
    int8_t saccadeX_ = 0;
    int8_t saccadeY_ = 0;
    uint32_t nextSaccadeMs_ = 0;

    // sway
    uint8_t swayFrames_ = 1;
    uint32_t swayAccumUs_ = 0;
    uint32_t lastSwayMs_ = 0;

    // lip sync
    uint8_t rawLip_ = 0;
    uint8_t smoothedLip_ = 0;
    uint32_t lastLipMs_ = 0;
};
