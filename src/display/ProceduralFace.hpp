#pragma once

#include <Arduino.h>

#include "AppTypes.hpp"

// Vector fallback face, used when no card is inserted or the pack is broken.
//
// It is deliberately crude compared to the SD artwork - its job is to keep the
// device usable and legible, not pretty. It allocates nothing and draws only
// dirty rectangles, so it also doubles as the safe mode renderer.
class ProceduralFace {
public:
    void begin();
    void invalidate() { staticDrawn_ = false; }
    void render(const FaceFrame& frame, bool showHint, const char* hint);

private:
    void drawScene();
    void drawFace(const FaceFrame& f);
    void drawEyes(const FaceFrame& f);
    void drawMouth(const FaceFrame& f);

    bool staticDrawn_ = false;
    Expression lastExpression_ = Expression::Count;
    EyeFrame lastEye_ = EyeFrame::Count;
    uint8_t lastViseme_ = 0xFF;
    int8_t lastGazeX_ = 99;
    int8_t lastGazeY_ = 99;
};
