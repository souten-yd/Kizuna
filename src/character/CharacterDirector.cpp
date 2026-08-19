#include "CharacterDirector.hpp"

#include "AppConfig.hpp"

namespace {

// The artist drew every stage of a blink, so play them: closing is faster
// than opening, which is what a real eyelid does and what makes a synthetic
// two-frame blink look mechanical by comparison.
constexpr EyeFrame kBlinkSequence[] = {
    EyeFrame::Half, EyeFrame::AlmostClosed, EyeFrame::Closed, EyeFrame::Closed,
    EyeFrame::AlmostClosed, EyeFrame::Half, EyeFrame::SoftLower, EyeFrame::Open,
};
constexpr uint8_t kBlinkSteps = sizeof(kBlinkSequence) / sizeof(kBlinkSequence[0]);

// Slots 0..4 track loudness; 5..7 are the shapes the packer reserves for
// rounded and smiling mouths.
constexpr uint8_t kVisemeSmileClosed = 6;
constexpr uint8_t kVisemeSmileOpen = 7;

}  // namespace

void CharacterDirector::begin(uint32_t nowMs) {
    frame_ = FaceFrame{};
    scheduleBlink(nowMs, false);
    nextSaccadeMs_ = nowMs + random(appcfg::kSaccadeMinMs, appcfg::kSaccadeMaxMs);
    lastSwayMs_ = nowMs;
    lastLipMs_ = nowMs;
}

void CharacterDirector::setState(CompanionState s) {
    if (s == state_) return;
    state_ = s;
    // Waking up should not inherit a half-finished blink from the sleep pose.
    if (s != CompanionState::Sleep) blinkStep_ = 0;
}

void CharacterDirector::setTilt(int8_t gazeX, int8_t gazeY) {
    tiltX_ = gazeX;
    tiltY_ = gazeY;
}

void CharacterDirector::setLipLevel(uint8_t level) {
    rawLip_ = level < appcfg::kVisemeCount ? level : appcfg::kVisemeCount - 1;
}

void CharacterDirector::setSwayFrameCount(uint8_t frames) {
    swayFrames_ = frames ? frames : 1;
}

void CharacterDirector::setDeviceStatus(uint8_t batteryPercent, bool charging, bool wifi,
                                        bool server, bool muted) {
    frame_.batteryPercent = batteryPercent;
    frame_.charging = charging;
    frame_.wifiOnline = wifi;
    frame_.serverOnline = server;
    frame_.muted = muted;
}

void CharacterDirector::scheduleBlink(uint32_t nowMs, bool soon) {
    nextBlinkMs_ = nowMs + (soon ? random(120, 320)
                                 : random(appcfg::kBlinkMinMs, appcfg::kBlinkMaxMs));
}

void CharacterDirector::poke(uint32_t nowMs) {
    saccadeX_ = 0;
    saccadeY_ = 0;
    nextSaccadeMs_ = nowMs + random(appcfg::kSaccadeMinMs, appcfg::kSaccadeMaxMs);
    if (!blinkStep_) scheduleBlink(nowMs, true);
}

void CharacterDirector::startle(uint32_t nowMs) {
    blinkStep_ = 0;
    frame_.eye = EyeFrame::Wide;
    // Hold the wide-eyed frame briefly by pushing the next blink out.
    nextBlinkMs_ = nowMs + 420;
    saccadeX_ = 0;
    saccadeY_ = -1;
}

void CharacterDirector::updateBlink(uint32_t nowMs) {
    if (state_ == CompanionState::Sleep) {
        frame_.eye = EyeFrame::Closed;
        blinkStep_ = 0;
        return;
    }
    if (expression_ == Expression::Sleepy && !blinkStep_) {
        // Drowsy eyes drift between half and fully shut on their own clock.
        frame_.eye = ((nowMs / 1700) % 3 == 0) ? EyeFrame::SleepyClosed : EyeFrame::SleepyHalf;
        return;
    }

    if (blinkStep_) {
        if (nowMs - blinkStepMs_ >= appcfg::kBlinkFrameMs) {
            blinkStepMs_ = nowMs;
            frame_.eye = kBlinkSequence[blinkStep_ - 1];
            if (++blinkStep_ > kBlinkSteps) {
                blinkStep_ = 0;
                frame_.eye = EyeFrame::Open;
                if (blinkRepeats_) {
                    --blinkRepeats_;
                    scheduleBlink(nowMs, true);
                } else {
                    scheduleBlink(nowMs, false);
                }
            }
        }
        return;
    }

    if (frame_.eye == EyeFrame::Wide && static_cast<int32_t>(nextBlinkMs_ - nowMs) > 0) return;

    if (static_cast<int32_t>(nowMs - nextBlinkMs_) >= 0) {
        blinkStep_ = 1;
        blinkStepMs_ = nowMs;
        frame_.eye = kBlinkSequence[0];
        if (!blinkRepeats_ && random(100) < static_cast<long>(appcfg::kDoubleBlinkPercent)) {
            blinkRepeats_ = 1;
        }
    } else {
        frame_.eye = EyeFrame::Open;
    }
}

void CharacterDirector::updateGaze(uint32_t nowMs) {
    // Attention states look straight at the user; idle states wander.
    const bool attentive = state_ == CompanionState::Listening ||
                           state_ == CompanionState::Speaking ||
                           state_ == CompanionState::Thinking;

    if (attentive) {
        saccadeX_ = 0;
        saccadeY_ = 0;
    } else if (static_cast<int32_t>(nowMs - nextSaccadeMs_) >= 0) {
        nextSaccadeMs_ = nowMs + random(appcfg::kSaccadeMinMs, appcfg::kSaccadeMaxMs);
        saccadeX_ = static_cast<int8_t>(random(-3, 4));
        saccadeY_ = static_cast<int8_t>(random(-2, 3));
    }

    const int gx = constrain(tiltX_ + saccadeX_, -4, 4);
    const int gy = constrain(tiltY_ + saccadeY_, -3, 3);
    frame_.gazeX = static_cast<int8_t>(gx);
    frame_.gazeY = static_cast<int8_t>(gy);
}

uint8_t CharacterDirector::swayFpsForState() const {
    switch (state_) {
        case CompanionState::Sleep: return 0;
        case CompanionState::Error: return 6;
        case CompanionState::Boot: return 8;
        default: break;
    }
    if (expression_ == Expression::Sleepy) return 6;
    return 12;
}

void CharacterDirector::updateSway(uint32_t nowMs) {
    const uint8_t fps = swayFpsForState();
    const uint32_t elapsed = nowMs - lastSwayMs_;
    lastSwayMs_ = nowMs;
    if (!fps || swayFrames_ <= 1) return;

    swayAccumUs_ += elapsed * 1000UL;
    const uint32_t stepUs = 1000000UL / fps;
    while (swayAccumUs_ >= stepUs) {
        swayAccumUs_ -= stepUs;
        frame_.swayFrame = static_cast<uint8_t>((frame_.swayFrame + 1) % swayFrames_);
    }
}

void CharacterDirector::updateViseme(uint32_t nowMs) {
    if (state_ != CompanionState::Speaking) {
        smoothedLip_ = 0;
        frame_.viseme = 0;
        lastLipMs_ = nowMs;
        return;
    }

    // Open fast, close slowly. A symmetric filter reads as chewing; this reads
    // as speech.
    if (rawLip_ > smoothedLip_) {
        smoothedLip_ = rawLip_;
        lastLipMs_ = nowMs;
    } else if (nowMs - lastLipMs_ >= 45) {
        if (smoothedLip_) --smoothedLip_;
        lastLipMs_ = nowMs;
    }

    // A happy face should keep smiling while it talks, so the loud visemes
    // route to the drawn smile shapes instead of the neutral ones.
    const bool smiling = expression_ == Expression::Happy || expression_ == Expression::Excited ||
                         expression_ == Expression::Playful;
    if (smiling && smoothedLip_ >= 3) {
        frame_.viseme = kVisemeSmileOpen;
    } else if (smiling && smoothedLip_ == 0) {
        frame_.viseme = kVisemeSmileClosed;
    } else {
        frame_.viseme = smoothedLip_;
    }
}

const FaceFrame& CharacterDirector::update(uint32_t nowMs) {
    frame_.expression = expression_;
    frame_.state = state_;
    updateBlink(nowMs);
    updateGaze(nowMs);
    updateSway(nowMs);
    updateViseme(nowMs);
    ++frame_.seq;
    return frame_;
}
