#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Companion state machine
// ---------------------------------------------------------------------------
enum class CompanionState : uint8_t {
    Boot,
    Idle,
    Listening,
    Thinking,
    Speaking,
    Sleep,
    Error,
    Count,
};

// Higher wins when two sources want the screen at the same time.
inline uint8_t statePriority(CompanionState s) {
    switch (s) {
        case CompanionState::Error:     return 90;
        case CompanionState::Speaking:  return 80;
        case CompanionState::Listening: return 70;
        case CompanionState::Thinking:  return 60;
        case CompanionState::Boot:      return 50;
        case CompanionState::Idle:      return 20;
        case CompanionState::Sleep:     return 10;
        default:                        return 0;
    }
}

// ---------------------------------------------------------------------------
// Expressions. The order matches the artwork sheet and the asset manifest.
// ---------------------------------------------------------------------------
enum class Expression : uint8_t {
    Neutral = 0,
    Happy,
    Excited,
    Thinking,
    Listening,
    Speaking,
    Confused,
    Sleepy,
    Playful,
    Error,
    Count,
};

constexpr uint8_t kExpressionCount = static_cast<uint8_t>(Expression::Count);

inline const char* expressionName(Expression e) {
    switch (e) {
        case Expression::Neutral:   return "neutral";
        case Expression::Happy:     return "happy";
        case Expression::Excited:   return "excited";
        case Expression::Thinking:  return "thinking";
        case Expression::Listening: return "listening";
        case Expression::Speaking:  return "speaking";
        case Expression::Confused:  return "confused";
        case Expression::Sleepy:    return "sleepy";
        case Expression::Playful:   return "playful";
        case Expression::Error:     return "error";
        default:                    return "neutral";
    }
}

inline const char* stateName(CompanionState s) {
    switch (s) {
        case CompanionState::Boot:      return "BOOT";
        case CompanionState::Idle:      return "IDLE";
        case CompanionState::Listening: return "LISTEN";
        case CompanionState::Thinking:  return "THINK";
        case CompanionState::Speaking:  return "SPEAK";
        case CompanionState::Sleep:     return "SLEEP";
        case CompanionState::Error:     return "ERROR";
        default:                        return "?";
    }
}

// ---------------------------------------------------------------------------
// Eye animation frames. The asset packer emits exactly these, in this order.
// ---------------------------------------------------------------------------
enum class EyeFrame : uint8_t {
    Open = 0,
    SoftLower,
    Half,
    AlmostClosed,
    Closed,
    Wide,
    SleepyHalf,
    SleepyClosed,
    Count,
};

constexpr uint8_t kEyeFrameCount = static_cast<uint8_t>(EyeFrame::Count);

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
enum class AppEventType : uint8_t {
    None = 0,
    NetworkConnected,
    NetworkDisconnected,
    ServerConnected,
    ServerDisconnected,
    PttPressed,
    PttReleased,
    ServerThinking,
    ServerIdle,
    SpeechBegin,
    SpeechEnd,
    ServerExpression,
    ServerGaze,
    MotionShake,
    MotionPickup,
    MotionStill,
    MuteToggled,
    SleepRequested,
    WakeRequested,
    AssetsReloaded,
    FatalError,
};

struct AppEvent {
    AppEventType type = AppEventType::None;
    int32_t value = 0;
    int32_t value2 = 0;
    uint32_t durationMs = 0;

    AppEvent() = default;
    explicit AppEvent(AppEventType t, int32_t v = 0, uint32_t d = 0, int32_t v2 = 0)
        : type(t), value(v), value2(v2), durationMs(d) {}
};

// ---------------------------------------------------------------------------
// One fully resolved animation frame, produced by CharacterDirector and
// consumed by the display task. Deliberately POD so it can travel in a queue.
// ---------------------------------------------------------------------------
struct FaceFrame {
    Expression expression = Expression::Neutral;
    CompanionState state = CompanionState::Boot;
    EyeFrame eye = EyeFrame::Open;
    uint8_t viseme = 0;        // 0..kVisemeCount-1
    uint8_t swayFrame = 0;     // index into the pre-rendered body sway clip
    int8_t gazeX = 0;          // -4..4 px pupil offset
    int8_t gazeY = 0;
    uint8_t batteryPercent = 0;
    bool charging = false;
    bool wifiOnline = false;
    bool serverOnline = false;
    bool muted = false;
    bool showDebug = false;
    uint32_t seq = 0;
};

struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    // An explicit constructor rather than default member initialisers: the
    // Arduino toolchain still compiles this as C++11, where the latter would
    // make Rect a non-aggregate and break brace initialisation.
    Rect() : x(0), y(0), w(0), h(0) {}
    Rect(int16_t x_, int16_t y_, int16_t w_, int16_t h_) : x(x_), y(y_), w(w_), h(h_) {}

    bool valid() const { return w > 0 && h > 0; }
    size_t bytes565() const { return static_cast<size_t>(w) * h * 2; }
};
