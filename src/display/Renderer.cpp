#include "Renderer.hpp"

#include <M5Unified.h>

#include "Board.hpp"
#include "TouchUi.hpp"

namespace {

constexpr uint16_t kPanelDark = 0x0841;
constexpr uint16_t kPanelLight = 0xEF5D;
constexpr uint16_t kOrange = 0xFBE0;
constexpr uint16_t kBlue = 0x4E7F;
constexpr uint16_t kDimOnDark = 0x8C71;
constexpr uint16_t kDimOnLight = 0x5AEB;
constexpr uint16_t kTextOnDark = 0xFFFF;
constexpr uint16_t kTextOnLight = 0x18C3;

}  // namespace

bool Renderer::begin(AssetPack* pack, TileCache* cache) {
    pack_ = pack;
    cache_ = cache;

    if (!band_) band_ = static_cast<uint8_t*>(malloc(appcfg::kBandBytes));
    if (!band_) {
        log_e("band buffer allocation failed");
        return false;
    }

    if (pack_ && pack_->ready()) {
        swapBytes_ = pack_->format() == m5a::kFormatRgb565Be;
        // Scratch large enough for the biggest cacheable tile.
        const size_t need = max(pack_->layerBytes(AssetPack::Layer::Eyes),
                                pack_->layerBytes(AssetPack::Layer::Mouth));
        if (cache_ && cache_->enabled() && need && need != tileBufBytes_) {
            free(tileBuf_);
            tileBuf_ = static_cast<uint8_t*>(malloc(need));
            tileBufBytes_ = tileBuf_ ? need : 0;
        }
    }

    M5.Display.setRotation(1);
    M5.Display.setSwapBytes(false);
    return true;
}

void Renderer::end() {
    free(band_);
    free(tileBuf_);
    band_ = nullptr;
    tileBuf_ = nullptr;
    tileBufBytes_ = 0;
}

void Renderer::pushTile(int16_t x, int16_t y, const uint8_t* data) const {
    // No startWrite/endWrite: the caller holds one transaction around a whole
    // batch. Taking and releasing the bus per tile costs more than the tile.
    if constexpr (board::kHasTouch) {
        x -= touchui::kContentShift;
        M5.Display.setClipRect(0, 0, touchui::kX, appcfg::kScreenH);
    }
    if (swapBytes_) {
        M5.Display.pushImage(x, y, m5a::kTileSide, m5a::kTileSide,
                             reinterpret_cast<const m5gfx::swap565_t*>(data));
    } else {
        M5.Display.pushImage(x, y, m5a::kTileSide, m5a::kTileSide,
                             reinterpret_cast<const m5gfx::rgb565_t*>(data));
    }
    if constexpr (board::kHasTouch) M5.Display.clearClipRect();
}

void Renderer::pushBand(int16_t x, int16_t y, int16_t w, int16_t rows, const uint8_t* data) const {
    if constexpr (board::kHasTouch) x -= touchui::kContentShift;
    M5.Display.startWrite();
    if constexpr (board::kHasTouch) {
        M5.Display.setClipRect(0, 0, touchui::kX, appcfg::kScreenH);
    }
    if (swapBytes_) {
        M5.Display.pushImage(x, y, w, rows, reinterpret_cast<const m5gfx::swap565_t*>(data));
    } else {
        M5.Display.pushImage(x, y, w, rows, reinterpret_cast<const m5gfx::rgb565_t*>(data));
    }
    if constexpr (board::kHasTouch) M5.Display.clearClipRect();
    M5.Display.endWrite();
}

void Renderer::noteDrawn(const Rect& r) {
    if (!r.valid()) return;
    const bool overlaps = r.x < overlayRect_.x + overlayRect_.w &&
                          r.x + r.w > overlayRect_.x &&
                          r.y < overlayRect_.y + overlayRect_.h &&
                          r.y + r.h > overlayRect_.y;
    if (overlaps) overlayDirty_ = true;
}

void Renderer::requestBase(Expression e) {
    baseExpr_ = e;
    baseRow_ = 0;
}

size_t Renderer::stepBase(FrameBudget& budget) {
    if (!basePending() || !pack_ || !pack_->ready() || !band_) return 0;

    const Rect r = pack_->layerRect(AssetPack::Layer::Base);
    const size_t rowBytes = static_cast<size_t>(r.w) * 2;
    size_t moved = 0;

    while (baseRow_ < r.h) {
        const int16_t rows = min<int16_t>(appcfg::kBandRows, r.h - baseRow_);
        // FrameBudget already prices both halves of the trip - its
        // nanoseconds-per-byte figure is the SD read plus the LCD write - so
        // the cost here is the tile size, not twice it.
        const size_t bytes = rowBytes * rows;
        if (!budget.take(bytes)) break;

        if (!pack_->readBand(baseExpr_, AssetPack::Layer::Base, 0, baseRow_, rows, band_)) {
            // A read failure must not wedge the renderer: give up on this
            // repaint and let the next expression change try again.
            baseRow_ = r.h;
            log_w("base band read failed at row %d", baseRow_);
            break;
        }
        pushBand(r.x, r.y + baseRow_, r.w, rows, band_);
        baseRow_ += rows;
        moved += bytes;
    }

    if (!basePending()) {
        overlayDirty_ = true;
    }
    return moved;
}

size_t Renderer::drawTile(Expression e, AssetPack::Layer layer, uint16_t frame) {
    if (!pack_ || !pack_->ready() || !band_) return 0;
    const Rect r = pack_->layerRect(layer);
    if (!r.valid()) return 0;

    const uint32_t key = TileCache::makeKey(e, static_cast<uint8_t>(layer), frame);

    if (cache_ && cache_->enabled()) {
        if (const uint8_t* hit = cache_->get(key)) {
            pushBand(r.x, r.y, r.w, r.h, hit);
            noteDrawn(r);
            return r.bytes565();
        }
        // Fill through the cache when the tile fits the scratch buffer.
        if (tileBuf_ && r.bytes565() <= tileBufBytes_ && r.bytes565() <= cache_->entryBytes()) {
            if (pack_->readFrame(e, layer, frame, tileBuf_, tileBufBytes_)) {
                pushBand(r.x, r.y, r.w, r.h, tileBuf_);
                if (uint8_t* slot = cache_->reserve(key)) {
                    memcpy(slot, tileBuf_, r.bytes565());
                    cache_->commit(slot);
                }
                noteDrawn(r);
                return r.bytes565();
            }
            return 0;
        }
    }

    // Streaming path: band by band, straight from the card to the panel.
    const size_t rowBytes = static_cast<size_t>(r.w) * 2;
    const int16_t bandRows = max<int16_t>(1, static_cast<int16_t>(appcfg::kBandBytes / rowBytes));
    size_t moved = 0;
    for (int16_t row = 0; row < r.h; row += bandRows) {
        const int16_t rows = min<int16_t>(bandRows, r.h - row);
        if (!pack_->readBand(e, layer, frame, row, rows, band_)) return moved;
        pushBand(r.x, r.y + row, r.w, rows, band_);
        moved += rowBytes * rows;
    }
    noteDrawn(r);
    return moved;
}

size_t Renderer::drawClipFrame(const char* clip, uint16_t frame) {
    if (!pack_ || !pack_->ready() || !band_ || !pack_->hasClip(clip)) return 0;

    if (pack_->clipIsTileDelta(clip)) return drawClipFrameTiles(clip, frame);

    uint16_t width = 0;
    const int16_t bandRows = appcfg::kBandRows;
    size_t moved = 0;
    for (int16_t row = 0; row < appcfg::kScreenH; row += bandRows) {
        const int16_t rows = min<int16_t>(bandRows, appcfg::kScreenH - row);
        if (!pack_->readClipBand(clip, frame, row, rows, band_, width) || !width) return moved;
        pushBand(0, row, width, rows, band_);
        moved += static_cast<size_t>(width) * 2 * rows;
    }
    overlayDirty_ = true;
    return moved;
}

size_t Renderer::drawClipFrameTiles(const char* clip, uint16_t frame) {
    static uint16_t indices[m5a::kMaxTilesPerFrame];
    uint16_t count = 0;
    uint16_t tilesX = 0;
    if (!pack_->readClipTileIndex(clip, frame, indices, m5a::kMaxTilesPerFrame,
                                  count, tilesX) || !tilesX) {
        return 0;
    }

    // As many tiles per read as the band buffer holds: the seek dominates a
    // 512 byte tile, so batching is most of the win over reading them singly.
    const uint16_t batch = max<uint16_t>(1, appcfg::kBandBytes / m5a::kTileBytes);
    size_t moved = 0;
    for (uint16_t done = 0; done < count; done += batch) {
        const uint16_t n = min<uint16_t>(batch, count - done);
        if (!pack_->readClipTileData(clip, frame, done, n, band_)) break;
        // One transaction for the batch, and none held across the SD read
        // above - the card and the panel are on the same bus.
        M5.Display.startWrite();
        for (uint16_t i = 0; i < n; ++i) {
            const uint16_t tile = indices[done + i];
            pushTile((tile % tilesX) * m5a::kTileSide, (tile / tilesX) * m5a::kTileSide,
                     band_ + i * m5a::kTileBytes);
        }
        M5.Display.endWrite();
        moved += static_cast<size_t>(n) * m5a::kTileBytes;
    }
    overlayDirty_ = true;
    return moved;
}

void Renderer::drawTouchBar(const FaceFrame& frame) {
    if constexpr (!board::kHasTouch) return;

    const uint32_t signature = frame.touchAction |
                               (static_cast<uint32_t>(frame.volumeStep) << 8) |
                               (static_cast<uint32_t>(frame.volumeSteps) << 12) |
                               (static_cast<uint32_t>(frame.muted) << 16) |
                               (static_cast<uint32_t>(frame.state == CompanionState::Listening)
                                << 17) |
                               (static_cast<uint32_t>(frame.state == CompanionState::Sleep) << 18);
    if (!touchBarDirty_ && signature == touchBarSignature_) return;

    auto& d = M5.Display;
    constexpr uint16_t kBar = 0x0863;
    constexpr uint16_t kTile = 0x10C6;
    constexpr uint16_t kPressed = 0x194B;
    constexpr uint16_t kEdge = 0x298A;
    constexpr uint16_t kIcon = 0xDEFF;
    constexpr uint16_t kCyan = 0x369F;
    constexpr uint16_t kViolet = 0xA35F;
    constexpr uint16_t kAmber = 0xFD00;

    d.startWrite();
    d.fillRect(touchui::kX, 0, touchui::kWidth, appcfg::kScreenH, kBar);
    d.drawFastVLine(touchui::kX, 0, appcfg::kScreenH, 0x31CD);
    d.drawFastVLine(touchui::kX + 1, 0, appcfg::kScreenH, 0x10A6);

    for (uint8_t i = 0; i < touchui::kItemCount; ++i) {
        const int16_t y = i * touchui::kItemHeight;
        bool active = frame.touchAction == i + 1;
        if (i == 0) active = active || frame.state == CompanionState::Listening;
        if (i == 1) active = active || frame.muted;
        if (i == 2) active = active || frame.state == CompanionState::Sleep;
        const uint16_t accent = i == 3 ? kViolet : (i == 2 ? kAmber : kCyan);
        d.fillRoundRect(touchui::kX + 5, y + 5, touchui::kWidth - 10,
                        touchui::kItemHeight - 10, 14, active ? kPressed : kTile);
        d.drawRoundRect(touchui::kX + 5, y + 5, touchui::kWidth - 10,
                        touchui::kItemHeight - 10, 14, active ? accent : kEdge);
        // A short illuminated spine gives the rail a hardware-control feel
        // without adding labels that would be unreadable at this size.
        if (active) d.fillRoundRect(touchui::kX + 7, y + 20, 3, 20, 2, accent);
    }

    const int16_t cx = touchui::kX + touchui::kWidth / 2;

    // Talk: broadcast microphone with a live-status jewel.
    uint16_t color = frame.state == CompanionState::Listening ? kCyan : kIcon;
    d.drawRoundRect(cx - 6, 15, 12, 20, 6, color);
    d.drawFastHLine(cx - 3, 20, 6, color);
    d.drawFastHLine(cx - 3, 24, 6, color);
    d.drawArc(cx, 29, 12, 11, 0, 180, color);
    d.drawFastVLine(cx, 40, 5, color);
    d.fillRoundRect(cx - 6, 44, 12, 2, 1, color);
    d.fillCircle(cx + 13, 17, 2, frame.state == CompanionState::Listening ? kAmber : kEdge);

    // Sound: one control carries both level and mute, matching its gestures.
    const int16_t volumeY = 88;
    color = frame.muted ? kAmber : kIcon;
    d.fillRoundRect(cx - 14, volumeY - 5, 6, 10, 2, color);
    d.fillTriangle(cx - 8, volumeY - 5, cx, volumeY - 13, cx, volumeY + 13, color);
    if (frame.muted) {
        d.drawLine(cx + 5, volumeY - 8, cx + 15, volumeY + 8, kAmber);
        d.drawLine(cx + 15, volumeY - 8, cx + 5, volumeY + 8, kAmber);
    } else {
        d.drawArc(cx + 1, volumeY, 9, 7, 270, 90, color);
        d.drawArc(cx + 1, volumeY, 15, 13, 270, 90, color);
    }
    const uint8_t level = frame.volumeSteps
                              ? min<uint8_t>(frame.volumeStep + 1, frame.volumeSteps)
                              : 1;
    for (uint8_t i = 0; i < appcfg::kVolumeStepCount; ++i) {
        const uint16_t dot = !frame.muted && i < level ? kCyan : kEdge;
        d.fillCircle(cx - 10 + i * 5, 109, 1, dot);
    }

    // Brightness: solar aperture. Holding it turns the panel fully off.
    const int16_t sunY = 150;
    color = frame.state == CompanionState::Sleep ? kAmber : kIcon;
    d.drawCircle(cx, sunY, 8, color);
    d.fillCircle(cx, sunY, 4, color);
    d.drawFastVLine(cx, sunY - 16, 5, color);
    d.drawFastVLine(cx, sunY + 12, 5, color);
    d.drawFastHLine(cx - 16, sunY, 5, color);
    d.drawFastHLine(cx + 12, sunY, 5, color);
    d.drawLine(cx - 11, sunY - 11, cx - 8, sunY - 8, color);
    d.drawLine(cx + 8, sunY + 8, cx + 11, sunY + 11, color);
    d.drawLine(cx + 8, sunY - 8, cx + 11, sunY - 11, color);
    d.drawLine(cx - 11, sunY + 11, cx - 8, sunY + 8, color);

    // Settings: a precise gear with a violet hub, visually distinct from the
    // three immediate controls. Holding it remains the guarded reset gesture.
    const int16_t gearY = 210;
    d.drawCircle(cx, gearY, 11, kIcon);
    d.drawCircle(cx, gearY, 10, kViolet);
    d.fillCircle(cx, gearY, 4, kIcon);
    d.fillCircle(cx, gearY, 2, kBar);
    d.drawFastVLine(cx, gearY - 16, 5, kIcon);
    d.drawFastVLine(cx, gearY + 12, 5, kIcon);
    d.drawFastHLine(cx - 16, gearY, 5, kIcon);
    d.drawFastHLine(cx + 12, gearY, 5, kIcon);
    d.drawLine(cx - 12, gearY - 12, cx - 8, gearY - 8, kIcon);
    d.drawLine(cx + 8, gearY + 8, cx + 12, gearY + 12, kIcon);
    d.drawLine(cx + 8, gearY - 8, cx + 12, gearY - 12, kIcon);
    d.drawLine(cx - 12, gearY + 12, cx - 8, gearY + 8, kIcon);
    d.fillRoundRect(cx - 8, 230, 16, 2, 1, kViolet);

    d.endWrite();
    touchBarDirty_ = false;
    touchBarSignature_ = signature;
}

void Renderer::drawOverlay(const FaceFrame& frame, const FrameBudget& budget, uint32_t fpsX10) {
    auto& d = M5.Display;
    const bool light = pack_ && pack_->ready() && pack_->lightTheme();
    const uint16_t panel = light ? kPanelLight : kPanelDark;
    const uint16_t dim = light ? kDimOnLight : kDimOnDark;

    d.startWrite();

    const int16_t y = overlayRect_.y;
    d.fillRect(0, y, board::kHasTouch ? touchui::kX : appcfg::kScreenW,
               overlayRect_.h, panel);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    const uint16_t linkColor = frame.serverOnline ? (light ? 0x0560 : 0x07E0)
                                                  : (frame.wifiOnline ? kOrange : 0xF800);
    d.fillCircle(10, y + 10, 4, linkColor);
    d.setTextColor(dim, panel);
    d.setCursor(20, y + 6);
    d.print(frame.serverOnline ? "ONLINE" : (frame.wifiOnline ? "NO SERVER" : "OFFLINE"));

    d.setCursor(96, y + 6);
    d.print(stateName(frame.state));

    if (frame.muted) {
        d.setTextColor(light ? 0xC980 : kOrange, panel);
        d.setCursor(160, y + 6);
        d.print("MUTE");
    }

    // Five segments between the mute label and the battery reading. Filled
    // ones are the level, the outlines are what is left, so the bar says both
    // where the volume is and how far it can still go - which is what the
    // button needs it to say, since one button walking a ring is otherwise
    // impossible to aim.
    // Not while the debug readout is up: that starts at 196 and runs straight
    // through the bar. Debug already replaces the battery reading, so it
    // replaces this too rather than being drawn on top of it.
    if (frame.volumeSteps && !frame.showDebug) {
        constexpr int16_t kBarX = 186;
        constexpr int16_t kSegW = 8;
        constexpr int16_t kSegGap = 2;
        const uint16_t on = frame.muted ? dim : (light ? 0xC980 : kOrange);
        for (uint8_t i = 0; i < frame.volumeSteps; ++i) {
            const int16_t sx = kBarX + i * (kSegW + kSegGap);
            // Taller as it goes up: a level is readable at a glance from the
            // shape even when the colours are hard to tell apart.
            const int16_t h = 4 + i * 2;
            const int16_t sy = y + 15 - h;
            if (i <= frame.volumeStep && !frame.muted) {
                d.fillRect(sx, sy, kSegW, h, on);
            } else {
                d.drawRect(sx, sy, kSegW, h, dim);
            }
        }
    }

    d.setTextColor(dim, panel);
    if (frame.showDebug) {
        d.setCursor(196, y + 6);
        d.printf("%u.%ufps %uk", fpsX10 / 10, fpsX10 % 10,
                 static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    } else {
        d.setCursor(238, y + 6);
        d.printf("%s%3d%%", frame.charging ? "+" : " ", frame.batteryPercent);
    }

    d.endWrite();
    overlayDirty_ = false;
}

void Renderer::drawMessage(const char* title, const char* line1, const char* line2,
                           uint16_t accent) {
    // Boot and failure screens are always dark: they may need to appear before
    // any pack has been read, so they cannot depend on one.
    auto& d = M5.Display;
    const uint16_t kPanel = kPanelDark;
    d.startWrite();
    d.fillScreen(kPanel);
    d.drawRoundRect(8, 8, appcfg::kScreenW - 16, appcfg::kScreenH - 16, 8, accent);
    d.setFont(&fonts::Font4);
    d.setTextColor(accent, kPanel);
    d.setCursor(24, 40);
    d.print(title ? title : "");
    d.setFont(&fonts::Font2);
    d.setTextColor(kTextOnDark, kPanel);
    if (line1) {
        d.setCursor(24, 96);
        d.print(line1);
    }
    if (line2) {
        d.setTextColor(kDimOnDark, kPanel);
        d.setCursor(24, 124);
        d.print(line2);
    }
    d.setFont(&fonts::Font0);
    d.endWrite();
    // Nothing of the pack is on screen any more.
    baseRow_ = appcfg::kScreenH;
    overlayDirty_ = true;
    touchBarDirty_ = true;
}

void Renderer::drawBootProgress(const char* step, uint8_t percent) {
    auto& d = M5.Display;
    const uint16_t kPanel = kPanelDark;
    d.startWrite();
    d.fillRect(24, 176, appcfg::kScreenW - 48, 40, kPanel);
    d.setFont(&fonts::Font0);
    d.setTextColor(kDimOnDark, kPanel);
    d.setCursor(24, 178);
    d.print(step ? step : "");
    d.drawRect(24, 194, appcfg::kScreenW - 48, 10, kBlue);
    const int16_t w = static_cast<int16_t>((appcfg::kScreenW - 52) * percent / 100);
    d.fillRect(26, 196, w, 6, kOrange);
    d.endWrite();
}
