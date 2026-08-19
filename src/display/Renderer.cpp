#include "Renderer.hpp"

#include <M5Unified.h>

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

void Renderer::pushBand(int16_t x, int16_t y, int16_t w, int16_t rows, const uint8_t* data) const {
    M5.Display.startWrite();
    if (swapBytes_) {
        M5.Display.pushImage(x, y, w, rows, reinterpret_cast<const m5gfx::swap565_t*>(data));
    } else {
        M5.Display.pushImage(x, y, w, rows, reinterpret_cast<const m5gfx::rgb565_t*>(data));
    }
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

void Renderer::drawOverlay(const FaceFrame& frame, const FrameBudget& budget, uint32_t fpsX10) {
    auto& d = M5.Display;
    const bool light = pack_ && pack_->ready() && pack_->lightTheme();
    const uint16_t panel = light ? kPanelLight : kPanelDark;
    const uint16_t dim = light ? kDimOnLight : kDimOnDark;

    d.startWrite();

    const int16_t y = overlayRect_.y;
    d.fillRect(0, y, appcfg::kScreenW, overlayRect_.h, panel);
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
