#include "ProceduralFace.hpp"

#include <M5Unified.h>

namespace {
constexpr uint16_t kBg = 0x0841;
constexpr uint16_t kPanel = 0x10A2;
constexpr uint16_t kOrange = 0xFBE0;
constexpr uint16_t kBlue = 0x4E7F;
constexpr uint16_t kHair = 0x2104;
constexpr uint16_t kHair2 = 0x39C7;
constexpr uint16_t kSkin = 0xFED9;
constexpr uint16_t kInk = 0x18C3;

constexpr int kFaceCx = 160;
constexpr int kFaceCy = 100;
constexpr int kEyeY = 102;
constexpr int kEyeLx = 137;
constexpr int kEyeRx = 183;
constexpr int kMouthY = 130;
}  // namespace

void ProceduralFace::begin() {
    M5.Display.setRotation(1);
    staticDrawn_ = false;
}

void ProceduralFace::drawScene() {
    auto& d = M5.Display;
    d.fillScreen(kBg);
    d.fillRoundRect(5, 5, 310, 214, 10, kPanel);
    d.drawRoundRect(5, 5, 310, 214, 10, 0x2F47);

    d.drawLine(12, 20, 60, 20, kBlue);
    d.fillCircle(64, 20, 3, kOrange);
    d.drawLine(256, 20, 306, 20, kOrange);
    d.fillCircle(252, 20, 3, kBlue);

    // hoodie + shoulders
    d.fillTriangle(58, 214, 95, 152, 133, 214, 0x1082);
    d.fillTriangle(187, 214, 225, 152, 263, 214, 0x1082);
    d.fillRoundRect(92, 155, 136, 62, 20, 0x18E3);
    d.drawLine(100, 162, 85, 214, kOrange);
    d.drawLine(220, 162, 235, 214, kOrange);
    d.fillTriangle(116, 157, 160, 187, 204, 157, 0xE73C);

    // neck + choker
    d.fillRoundRect(145, 139, 30, 35, 8, kSkin);
    d.fillRect(143, 157, 34, 7, kInk);
    d.fillCircle(160, 164, 3, kOrange);

    // hair mass behind the face
    d.fillEllipse(160, 70, 82, 61, kHair);
    d.fillTriangle(84, 73, 112, 39, 108, 123, kHair);
    d.fillTriangle(236, 73, 208, 39, 212, 123, kHair);

    // M5 hair clip
    d.fillRoundRect(201, 48, 29, 18, 3, 0x2124);
    d.drawRoundRect(201, 48, 29, 18, 3, kOrange);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xFFFF, 0x2124);
    d.setCursor(207, 53);
    d.print("M5");

    staticDrawn_ = true;
}

void ProceduralFace::drawEyes(const FaceFrame& f) {
    auto& d = M5.Display;
    const bool closed = f.eye == EyeFrame::Closed || f.expression == Expression::Sleepy;
    const bool half = f.eye == EyeFrame::Half;
    const int radiusY = f.eye == EyeFrame::Wide ? 18 : (half ? 9 : 16);

    for (int i = 0; i < 2; ++i) {
        const int x = i == 0 ? kEyeLx : kEyeRx;
        const bool wink = f.expression == Expression::Playful && i == 0;
        if (closed || wink) {
            d.drawLine(x - 12, kEyeY, x + 12, kEyeY + (i == 0 ? 1 : -1), kInk);
            continue;
        }
        d.fillEllipse(x, kEyeY, 13, radiusY, 0xFFFF);
        d.drawEllipse(x, kEyeY, 13, radiusY, kInk);
        const int py = kEyeY + f.gazeY;
        const int px = x + f.gazeX;
        d.fillCircle(px, py, 7, 0x5BD2);
        d.fillCircle(px + 1, py, 3, kInk);
        d.fillCircle(px - 2, py - 3, 2, 0xFFFF);
    }

    int brow = 0;
    if (f.expression == Expression::Confused || f.expression == Expression::Error) brow = -3;
    if (f.expression == Expression::Excited) brow = 2;
    d.drawLine(kEyeLx - 11, 80 + brow, kEyeLx + 9, 78 - brow, kInk);
    d.drawLine(kEyeRx - 9, 78 - brow, kEyeRx + 11, 80 + brow, kInk);
}

void ProceduralFace::drawMouth(const FaceFrame& f) {
    auto& d = M5.Display;
    const int x = kFaceCx;
    const int y = kMouthY;

    if (f.viseme > 0) {
        const int h = 2 + f.viseme * 2;
        d.fillEllipse(x, y + 2, 6 + f.viseme, h, 0x6A0D);
        d.drawEllipse(x, y + 2, 6 + f.viseme, h, kInk);
        return;
    }

    switch (f.expression) {
        case Expression::Happy:
        case Expression::Playful:
            d.drawLine(x - 10, y - 1, x, y + 5, kInk);
            d.drawLine(x, y + 5, x + 10, y - 1, kInk);
            break;
        case Expression::Excited:
            d.fillEllipse(x, y + 3, 9, 6, 0x9B0D);
            d.drawEllipse(x, y + 3, 9, 6, kInk);
            break;
        case Expression::Error:
            d.drawLine(x - 7, y + 4, x, y - 1, kInk);
            d.drawLine(x, y - 1, x + 7, y + 4, kInk);
            break;
        case Expression::Sleepy:
            d.fillCircle(x, y + 3, 4, kInk);
            break;
        case Expression::Confused:
        case Expression::Thinking:
            d.drawLine(x - 5, y + 1, x + 5, y, kInk);
            break;
        default:
            d.drawLine(x - 6, y, x + 6, y, kInk);
            break;
    }
}

void ProceduralFace::drawFace(const FaceFrame& f) {
    auto& d = M5.Display;
    d.startWrite();
    d.fillEllipse(kFaceCx, kFaceCy, 69, 63, kSkin);
    d.fillTriangle(95, 70, 111, 53, 107, 127, kHair);
    d.fillTriangle(225, 70, 209, 53, 213, 127, kHair);
    d.fillTriangle(105, 55, 140, 45, 125, 89, kHair2);
    d.fillTriangle(130, 43, 162, 38, 147, 87, kHair);
    d.fillTriangle(157, 37, 188, 44, 171, 88, kHair2);
    d.fillTriangle(183, 46, 211, 61, 188, 92, kHair);

    drawEyes(f);
    drawMouth(f);

    switch (f.expression) {
        case Expression::Listening:
            d.drawCircle(236, 102, 6, kBlue);
            d.drawCircle(236, 102, 10, kBlue);
            break;
        case Expression::Thinking:
            d.fillCircle(236, 88, 2, kBlue);
            d.fillCircle(243, 81, 3, kBlue);
            break;
        case Expression::Excited:
            d.drawLine(234, 80, 242, 73, kOrange);
            d.drawLine(238, 87, 249, 85, kOrange);
            break;
        case Expression::Confused:
            d.setFont(&fonts::Font2);
            d.setTextColor(kBlue, kPanel);
            d.setCursor(236, 70);
            d.print('?');
            d.setFont(&fonts::Font0);
            break;
        default:
            break;
    }
    d.endWrite();
}

void ProceduralFace::render(const FaceFrame& f, bool showHint, const char* hint) {
    if (!staticDrawn_) {
        drawScene();
        lastExpression_ = Expression::Count;
        if (showHint && hint && *hint) {
            M5.Display.setFont(&fonts::Font0);
            M5.Display.setTextColor(kOrange, kPanel);
            M5.Display.setCursor(12, 200);
            M5.Display.print(hint);
        }
    }

    const bool dirty = f.expression != lastExpression_ || f.eye != lastEye_ ||
                       f.viseme != lastViseme_ || f.gazeX != lastGazeX_ || f.gazeY != lastGazeY_;
    if (!dirty) return;

    drawFace(f);
    lastExpression_ = f.expression;
    lastEye_ = f.eye;
    lastViseme_ = f.viseme;
    lastGazeX_ = f.gazeX;
    lastGazeY_ = f.gazeY;
}
