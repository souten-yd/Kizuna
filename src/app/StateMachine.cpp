#include "StateMachine.hpp"

#include "AppConfig.hpp"

namespace {
// If the server goes quiet after a request, do not sit in THINKING forever.
// Longer than anyone holds a button to speak - a 30 second utterance is
// already at the limit of what the recogniser handles well - and short enough
// that a device left stuck comes back on its own before the user gives up on it.
constexpr uint32_t kListeningTimeoutMs = 45000;
constexpr uint32_t kThinkingTimeoutMs = 20000;
// Likewise for a TTS stream whose speech.end never arrives.
constexpr uint32_t kSpeakingTimeoutMs = 60000;
constexpr uint32_t kBootHoldMs = 900;
}  // namespace

void StateMachine::begin(uint32_t nowMs) {
    state_ = CompanionState::Boot;
    stateEnteredMs_ = nowMs;
    lastInteractionMs_ = nowMs;
    hasOverride_ = false;
}

void StateMachine::setState(CompanionState next, uint32_t nowMs) {
    if (next == state_) return;
    // Leaving sleep by any route clears how it was entered, so the next sleep
    // is judged on its own.
    if (state_ == CompanionState::Sleep) deliberateSleep_ = false;
    state_ = next;
    stateEnteredMs_ = nowMs;
    // A conversational state speaks for itself; drop any decorative override.
    if (next != CompanionState::Idle) hasOverride_ = false;
}

void StateMachine::setExpressionOverride(Expression e, uint32_t durationMs, uint32_t nowMs) {
    override_ = e;
    overrideUntilMs_ = nowMs + (durationMs ? durationMs : 1500);
    hasOverride_ = true;
}

bool StateMachine::handle(const AppEvent& event, uint32_t nowMs) {
    const CompanionState before = state_;

    switch (event.type) {
        case AppEventType::NetworkConnected:
            wifiOnline_ = true;
            if (state_ == CompanionState::Boot) setState(CompanionState::Idle, nowMs);
            break;

        case AppEventType::NetworkDisconnected:
            wifiOnline_ = false;
            serverOnline_ = false;
            if (state_ != CompanionState::Sleep) setExpressionOverride(Expression::Confused, 2200, nowMs);
            if (state_ == CompanionState::Boot) setState(CompanionState::Idle, nowMs);
            if (state_ == CompanionState::Thinking || state_ == CompanionState::Speaking) {
                setState(CompanionState::Idle, nowMs);
            }
            break;

        case AppEventType::ServerConnected:
            serverOnline_ = true;
            if (state_ == CompanionState::Boot) setState(CompanionState::Idle, nowMs);
            setExpressionOverride(Expression::Happy, 1200, nowMs);
            break;

        case AppEventType::ServerDisconnected:
            serverOnline_ = false;
            if (state_ == CompanionState::Thinking || state_ == CompanionState::Speaking) {
                setState(CompanionState::Idle, nowMs);
            }
            if (state_ != CompanionState::Sleep) setExpressionOverride(Expression::Confused, 2000, nowMs);
            break;

        case AppEventType::PttPressed:
            noteInteraction(nowMs);
            if (state_ == CompanionState::Sleep) {
                setState(CompanionState::Idle, nowMs);
            } else {
                setState(CompanionState::Listening, nowMs);
            }
            break;

        case AppEventType::PttReleased:
            noteInteraction(nowMs);
            if (state_ == CompanionState::Listening) {
                // Without a server there is nothing to wait for.
                setState(serverOnline_ ? CompanionState::Thinking : CompanionState::Idle, nowMs);
                if (!serverOnline_) setExpressionOverride(Expression::Confused, 1600, nowMs);
            }
            break;

        case AppEventType::ServerThinking:
            if (state_ != CompanionState::Sleep) setState(CompanionState::Thinking, nowMs);
            break;

        case AppEventType::ServerIdle:
            if (state_ == CompanionState::Thinking) setState(CompanionState::Idle, nowMs);
            break;

        case AppEventType::SpeechBegin:
            if (state_ != CompanionState::Sleep) setState(CompanionState::Speaking, nowMs);
            break;

        case AppEventType::SpeechEnd:
            if (state_ == CompanionState::Speaking) setState(CompanionState::Idle, nowMs);
            break;

        case AppEventType::ServerExpression:
            if (state_ != CompanionState::Sleep) {
                setExpressionOverride(static_cast<Expression>(event.value), event.durationMs, nowMs);
            }
            break;

        case AppEventType::MotionShake:
            noteInteraction(nowMs);
            if (state_ == CompanionState::Sleep) {
                if (motionMayWake(nowMs)) setState(CompanionState::Idle, nowMs);
            } else if (state_ == CompanionState::Idle) {
                setExpressionOverride(Expression::Excited, 1100, nowMs);
            }
            break;

        case AppEventType::MotionPickup:
            noteInteraction(nowMs);
            if (state_ == CompanionState::Sleep) {
                if (motionMayWake(nowMs)) setState(CompanionState::Idle, nowMs);
            } else if (state_ == CompanionState::Idle) {
                setExpressionOverride(Expression::Playful, 1400, nowMs);
            }
            break;

        case AppEventType::SleepRequested:
            // A sleep someone asked for is not the same as a sleep the device
            // decided on, and only one of them should be undone by movement.
            //
            // Letting go of the button that turns the screen off moves the
            // device, the accelerometer calls that a pickup, and the screen
            // came back in the same gesture that darkened it. Worse, a
            // companion switched off on purpose should stay off while the desk
            // is bumped around it. So deliberate sleep answers to buttons only.
            // Sleep the device fell into on its own still wakes to a hand,
            // which is the whole point of it.
            deliberateSleep_ = true;
            setState(CompanionState::Sleep, nowMs);
            break;

        case AppEventType::WakeRequested:
            noteInteraction(nowMs);
            setState(CompanionState::Idle, nowMs);
            break;

        case AppEventType::FatalError:
            setState(CompanionState::Error, nowMs);
            break;

        default:
            break;
    }

    return state_ != before;
}

void StateMachine::update(uint32_t nowMs) {
    if (hasOverride_ && static_cast<int32_t>(nowMs - overrideUntilMs_) >= 0) hasOverride_ = false;

    switch (state_) {
        case CompanionState::Boot:
            if (nowMs - stateEnteredMs_ > kBootHoldMs) setState(CompanionState::Idle, nowMs);
            break;
        case CompanionState::Listening:
            // Listening is entered by holding a button and left by releasing
            // it, so it should not be able to outlive the press - and it did:
            // the device sat showing "listening" with nobody touching it,
            // whenever the release went missing because the loop was busy or
            // the link had gone. Every other state here is already bounded;
            // this one was not, and a state with no way out is a state you will
            // eventually be stuck in.
            if (nowMs - stateEnteredMs_ > kListeningTimeoutMs) {
                setState(CompanionState::Idle, nowMs);
                setExpressionOverride(Expression::Confused, 1800, nowMs);
            }
            break;
        case CompanionState::Thinking:
            if (nowMs - stateEnteredMs_ > kThinkingTimeoutMs) {
                setState(CompanionState::Idle, nowMs);
                setExpressionOverride(Expression::Confused, 1800, nowMs);
            }
            break;
        case CompanionState::Speaking:
            if (nowMs - stateEnteredMs_ > kSpeakingTimeoutMs) setState(CompanionState::Idle, nowMs);
            break;
        case CompanionState::Idle:
            if (nowMs - lastInteractionMs_ > appcfg::kSleepyToSleepMs) {
                deliberateSleep_ = false;   // it chose this; a hand may undo it
                setState(CompanionState::Sleep, nowMs);
            }
            break;
        case CompanionState::Error:
            // Recover on its own once the link is healthy again.
            if (serverOnline_ && nowMs - stateEnteredMs_ > 4000) setState(CompanionState::Idle, nowMs);
            break;
        default:
            break;
    }
}

Expression StateMachine::restingExpression() const {
    switch (state_) {
        case CompanionState::Listening: return Expression::Listening;
        case CompanionState::Thinking:  return Expression::Thinking;
        case CompanionState::Speaking:  return Expression::Speaking;
        case CompanionState::Sleep:     return Expression::Sleepy;
        case CompanionState::Error:     return Expression::Error;
        default: break;
    }
    // Long, undisturbed idle drifts into a drowsy face before actually
    // sleeping - the transition should be visible, not abrupt.
    if (millis() - lastInteractionMs_ > appcfg::kIdleToSleepyMs) return Expression::Sleepy;
    return Expression::Neutral;
}

Expression StateMachine::expression() const {
    return hasOverride_ ? override_ : restingExpression();
}
