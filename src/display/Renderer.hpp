#pragma once

#include <Arduino.h>

#include "AppConfig.hpp"
#include "AppTypes.hpp"
#include "AssetPack.hpp"
#include "FrameBudget.hpp"
#include "TileCache.hpp"

// Draws the companion. Runs only on the display task, which owns the SPI bus.
//
// Everything is a rectangle blit of pre-rendered RGB565: no decoding, no
// alpha compositing, no full-screen frame buffer. The single 10 KiB band
// buffer below is the entire drawing working set.
class Renderer {
public:
    bool begin(AssetPack* pack, TileCache* cache);
    void end();

    // A full-screen base repaint is spread over several ticks so a slow card
    // cannot freeze the loop. While one is in flight, tiles are suppressed
    // because they would land on rows that have not been painted yet.
    void requestBase(Expression e);
    bool basePending() const { return baseRow_ < appcfg::kScreenH; }
    Expression baseExpression() const { return baseExpr_; }
    size_t stepBase(FrameBudget& budget);

    // Returns the number of bytes moved (0 when nothing was drawn).
    size_t drawTile(Expression e, AssetPack::Layer layer, uint16_t frame);

    void markOverlayDirty() { overlayDirty_ = true; }
    bool overlayDirty() const { return overlayDirty_; }
    void drawOverlay(const FaceFrame& frame, const FrameBudget& budget, uint32_t fpsX10);
    bool touchBarDirty() const { return touchBarDirty_; }
    void drawTouchBar(const FaceFrame& frame);

    // Text screens for boot progress and unrecoverable states. These bypass
    // the asset pack entirely so they still work with no card inserted.
    void drawMessage(const char* title, const char* line1, const char* line2, uint16_t accent);
    void drawBootProgress(const char* step, uint8_t percent);

    size_t drawClipFrame(const char* clip, uint16_t frame);


private:
    size_t drawClipFrameTiles(const char* clip, uint16_t frame);
    // Assumes a write transaction is already open; see drawClipFrameTiles.
    void pushTile(int16_t x, int16_t y, const uint8_t* data) const;
    void pushBand(int16_t x, int16_t y, int16_t w, int16_t rows, const uint8_t* data) const;
    void noteDrawn(const Rect& r);

    AssetPack* pack_ = nullptr;
    TileCache* cache_ = nullptr;
    uint8_t* band_ = nullptr;
    uint8_t* tileBuf_ = nullptr;      // scratch for cache fills
    size_t tileBufBytes_ = 0;
    bool swapBytes_ = true;           // true when assets are panel-native BE

    Expression baseExpr_ = Expression::Neutral;
    int16_t baseRow_ = appcfg::kScreenH;

    bool overlayDirty_ = true;
    Rect overlayRect_{0, appcfg::kScreenH - 20, appcfg::kScreenW, 20};
    bool touchBarDirty_ = true;
    uint32_t touchBarSignature_ = UINT32_MAX;
};
