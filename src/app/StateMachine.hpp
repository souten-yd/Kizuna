#pragma once

#include <Arduino.h>

#include "AppTypes.hpp"

// Owns "what is the companion doing", separate from "what does it look like".
//
// Expression overrides are transient reactions layered on top of the state's
// resting face, which is why a happy flash can be interrupted by a button
// press without any special casing.
class StateMachine {
public:
    void begin(uint32_t nowMs);
    bool handle(const AppEvent& event, uint32_t nowMs);
    void update(uint32_t nowMs);

    CompanionState state() const { return state_; }
    Expression expression() const;
    bool offline() const { return !serverOnline_; }

    void setExpressionOverride(Expression e, uint32_t durationMs, uint32_t nowMs);
    void noteInteraction(uint32_t nowMs) { lastInteractionMs_ = nowMs; }

    // Set by the app so the state machine can decide whether a PTT release
    // should wait for a server reply or bounce straight back to idle.
    void setServerOnline(bool online) { serverOnline_ = online; }
    void setWifiOnline(bool online) { wifiOnline_ = online; }

private:
    void setState(CompanionState next, uint32_t nowMs);
    bool motionMayWake(uint32_t) const { return !deliberateSleep_; }
    Expression restingExpression() const;

    CompanionState state_ = CompanionState::Boot;
    Expression override_ = Expression::Neutral;
    uint32_t overrideUntilMs_ = 0;
    bool hasOverride_ = false;

    bool serverOnline_ = false;
    bool wifiOnline_ = false;

    uint32_t stateEnteredMs_ = 0;
    bool deliberateSleep_ = false;
    uint32_t lastInteractionMs_ = 0;
};
