#include "AssetPack.hpp"

#include <ArduinoJson.h>
#include <SD.h>

namespace {

Rect readRect(JsonObjectConst obj) {
    Rect r;
    r.x = obj["x"] | 0;
    r.y = obj["y"] | 0;
    r.w = obj["w"] | 0;
    r.h = obj["h"] | 0;
    return r;
}

String join(const String& root, const char* rel) {
    if (!rel || !*rel) return String();
    if (rel[0] == '/') return String(rel);
    return root + "/" + rel;
}

const char* kLayerKey[AssetPack::kLayerCount] = {"base", "sway", "eyes", "mouth"};

}  // namespace

uint16_t AssetPack::swayIndex(uint8_t sway) const {
    return sway % (swayFrames_ ? swayFrames_ : 1);
}

uint16_t AssetPack::eyeIndex(uint8_t sway, uint8_t slot) const {
    if (slot >= m5a::kEyeSlots) slot = m5a::kEyeOpenCenter;
    return static_cast<uint16_t>(swayIndex(sway) * m5a::kEyeSlots + slot);
}

uint16_t AssetPack::mouthIndex(uint8_t sway, uint8_t viseme) const {
    const uint8_t n = visemeFrames_ ? visemeFrames_ : 1;
    return static_cast<uint16_t>(swayIndex(sway) * n + (viseme % n));
}

bool AssetPack::load(const char* rootDir) {
    unload();
    if (!rootDir || !*rootDir) {
        error_ = "no pack directory";
        return false;
    }
    root_ = rootDir;

    const String manifestPath = root_ + "/manifest.json";
    File f = SD.open(manifestPath.c_str(), FILE_READ);
    if (!f) {
        error_ = "manifest.json not found";
        return false;
    }

    // Kizuna carries 30+ expressions, so give the manifest room to grow. The
    // document is temporary and is freed before Wi-Fi/audio settle into their
    // steady-state heap usage.
    DynamicJsonDocument doc(24576);
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        error_ = String("manifest parse: ") + err.c_str();
        return false;
    }

    name_ = doc["pack"] | "unnamed";
    const char* fmt = doc["format"] | "rgb565be";
    format_ = strcmp(fmt, "rgb565le") == 0 ? m5a::kFormatRgb565Le : m5a::kFormatRgb565Be;
    lightTheme_ = strcmp(doc["theme"] | "dark", "light") == 0;
    swayFrames_ = doc["sway_frames"] | 1;
    visemeFrames_ = doc["viseme_frames"] | 1;
    if (!swayFrames_) swayFrames_ = 1;
    if (!visemeFrames_) visemeFrames_ = 1;

    rect_[static_cast<uint8_t>(Layer::Base)] = {0, 0, appcfg::kScreenW, appcfg::kScreenH};
    rect_[static_cast<uint8_t>(Layer::Sway)] = readRect(doc["sway_rect"]);
    rect_[static_cast<uint8_t>(Layer::Eyes)] = readRect(doc["eye_rect"]);
    rect_[static_cast<uint8_t>(Layer::Mouth)] = readRect(doc["mouth_rect"]);

    JsonObjectConst expressions = doc["expressions"];
    if (expressions.isNull()) {
        error_ = "manifest has no expressions";
        return false;
    }

    // Resolve every expression, falling back to neutral so an older/partial
    // pack remains usable after the firmware learns new Kizuna expressions.
    for (uint8_t i = 0; i < kExpressionCount; ++i) {
        const char* key = expressionName(static_cast<Expression>(i));
        JsonObjectConst entry = expressions[key];
        if (entry.isNull()) entry = expressions["neutral"];
        if (entry.isNull()) continue;
        for (uint8_t l = 0; l < kLayerCount; ++l) {
            path_[i][l] = join(root_, entry[kLayerKey[l]] | "");
        }
    }

    if (path_[0][static_cast<uint8_t>(Layer::Base)].isEmpty()) {
        error_ = "neutral base clip missing";
        return false;
    }

    // Historically only the optional boot clip was understood by firmware,
    // even though the packer already emitted arbitrary gesture clips. Load all
    // named strings now so any recipe-defined one-shot can actually play.
    JsonObjectConst clips = doc["clips"];
    if (!clips.isNull()) {
        for (JsonPairConst kv : clips) {
            if (clipCount_ >= kMaxClips) {
                log_w("manifest has more than %u clips; extras ignored", kMaxClips);
                break;
            }
            const char* rel = kv.value().as<const char*>();
            if (!rel || !*rel) continue;
            clips_[clipCount_].name = kv.key().c_str();
            clips_[clipCount_].path = join(root_, rel);
            ++clipCount_;
        }
    }

    // Sway is the largest sequentially-read clip, so it is the honest
    // benchmark target for the frame budget.
    benchPath_ = path_[0][static_cast<uint8_t>(Layer::Sway)];
    if (benchPath_.isEmpty()) benchPath_ = path_[0][static_cast<uint8_t>(Layer::Base)];

    ready_ = true;
    error_ = "";
    log_i("pack '%s' loaded: sway=%u viseme=%u clips=%u eyes=%dx%d mouth=%dx%d", name_.c_str(),
          swayFrames_, visemeFrames_, clipCount_, rect_[2].w, rect_[2].h, rect_[3].w, rect_[3].h);
    return true;
}

void AssetPack::unload() {
    for (auto& slot : open_) {
        if (slot.file) slot.file.close();
        slot.path = "";
        slot.valid = false;
    }
    for (uint8_t i = 0; i < kExpressionCount; ++i)
        for (uint8_t l = 0; l < kLayerCount; ++l) path_[i][l] = "";
    for (uint8_t i = 0; i < kMaxClips; ++i) {
        clips_[i].name = "";
        clips_[i].path = "";
    }
    clipCount_ = 0;
    ready_ = false;
    name_ = "";
    root_ = "";
    benchPath_ = "";
}

const String& AssetPack::clipPath(Expression e, Layer l) const {
    const uint8_t idx = static_cast<uint8_t>(e) < kExpressionCount ? static_cast<uint8_t>(e) : 0;
    return path_[idx][static_cast<uint8_t>(l)];
}

const String* AssetPack::namedClipPath(const char* name) const {
    if (!name || !*name) return nullptr;
    for (uint8_t i = 0; i < clipCount_; ++i) {
        if (clips_[i].name == name) return &clips_[i].path;
    }
    return nullptr;
}

AssetPack::OpenFile* AssetPack::acquire(const String& path) {
    if (path.isEmpty()) return nullptr;

    OpenFile* victim = &open_[0];
    for (auto& slot : open_) {
        if (slot.valid && slot.path == path) {
            slot.lastUse = ++useCounter_;
            return &slot;
        }
        if (!slot.valid) {
            victim = &slot;
        } else if (victim->valid && slot.lastUse < victim->lastUse) {
            victim = &slot;
        }
    }

    if (victim->file) victim->file.close();
    victim->valid = false;
    victim->path = "";

    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return nullptr;

    m5a::Header h{};
    bool ok = f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) == static_cast<int>(sizeof(h)) &&
              h.magic == m5a::kMagic && h.version == m5a::kVersion && h.frameCount && h.width &&
              h.height;
    if (ok) {
        if (h.flags & m5a::kFlagTileDelta) {
            // frameBytes is the largest frame payload here, not a stride, so
            // it cannot be checked against the frame size - only bounded. The
            // keyframe carries every tile plus the index that names them, so
            // the bound is a whole screen *and* a full index, not just the
            // screen.
            const uint32_t tiles = (static_cast<uint32_t>(h.width) / m5a::kTileSide) *
                                   (static_cast<uint32_t>(h.height) / m5a::kTileSide);
            const uint32_t widest = sizeof(uint16_t) + tiles * sizeof(uint16_t) +
                                    tiles * m5a::kTileBytes;
            ok = h.frameBytes && h.frameBytes <= widest &&
                 tiles <= m5a::kMaxTilesPerFrame &&
                 (h.width % m5a::kTileSide) == 0 && (h.height % m5a::kTileSide) == 0;
        } else {
            ok = h.frameBytes == static_cast<uint32_t>(h.width) * h.height * 2;
        }
    }
    if (!ok) {
        f.close();
        log_w("bad m5a header: %s", path.c_str());
        return nullptr;
    }

    victim->file = f;
    victim->header = h;
    victim->path = path;
    victim->valid = true;
    victim->lastUse = ++useCounter_;
    return victim;
}

bool AssetPack::readBand(Expression e, Layer l, uint16_t frame, uint16_t rowStart, uint16_t rows,
                         uint8_t* dst) {
    if (!ready_ || !dst || !rows) return false;
    OpenFile* slot = acquire(clipPath(e, l));
    if (!slot) return false;

    const m5a::Header& h = slot->header;
    if (frame >= h.frameCount || rowStart + rows > h.height) return false;

    const size_t rowBytes = static_cast<size_t>(h.width) * 2;
    const size_t want = rowBytes * rows;
    const uint32_t offset =
        m5a::kHeaderBytes + static_cast<uint32_t>(frame) * h.frameBytes + rowStart * rowBytes;

    if (!slot->file.seek(offset)) return false;
    return slot->file.read(dst, want) == static_cast<int>(want);
}

bool AssetPack::readFrame(Expression e, Layer l, uint16_t frame, uint8_t* dst, size_t capacity) {
    if (!ready_ || !dst) return false;
    OpenFile* slot = acquire(clipPath(e, l));
    if (!slot) return false;

    const m5a::Header& h = slot->header;
    if (frame >= h.frameCount || h.frameBytes > capacity) return false;

    const uint32_t offset = m5a::kHeaderBytes + static_cast<uint32_t>(frame) * h.frameBytes;
    if (!slot->file.seek(offset)) return false;
    return slot->file.read(dst, h.frameBytes) == static_cast<int>(h.frameBytes);
}

bool AssetPack::hasClip(const char* name) const {
    return namedClipPath(name) != nullptr;
}

uint16_t AssetPack::clipFrameCount(const char* name) {
    const String* path = namedClipPath(name);
    if (!path) return 0;
    OpenFile* slot = acquire(*path);
    return slot ? slot->header.frameCount : 0;
}

uint16_t AssetPack::clipFps(const char* name) {
    const String* path = namedClipPath(name);
    if (!path) return 0;
    OpenFile* slot = acquire(*path);
    return slot ? slot->header.fps : 0;
}

namespace {

// Reads frame `frame`'s payload offset out of the table that follows the
// header. Two reads rather than one so a clip with many frames does not need
// its whole table in RAM.
bool frameOffset(File& file, uint16_t frame, uint32_t& start, uint32_t& end) {
    const uint32_t at = m5a::kHeaderBytes + static_cast<uint32_t>(frame) * sizeof(uint32_t);
    uint32_t pair[2];
    if (!file.seek(at)) return false;
    if (file.read(reinterpret_cast<uint8_t*>(pair), sizeof(pair)) != sizeof(pair)) return false;
    start = pair[0];
    end = pair[1];
    return end > start;
}

}  // namespace

bool AssetPack::clipIsTileDelta(const char* name) {
    const String* path = namedClipPath(name);
    if (!path) return false;
    OpenFile* slot = acquire(*path);
    return slot && (slot->header.flags & m5a::kFlagTileDelta);
}

bool AssetPack::readClipTileIndex(const char* name, uint16_t frame, uint16_t* indices,
                                  uint16_t maxTiles, uint16_t& outCount, uint16_t& outTilesX) {
    outCount = 0;
    if (!indices) return false;
    const String* path = namedClipPath(name);
    if (!path) return false;
    OpenFile* slot = acquire(*path);
    if (!slot) return false;

    const m5a::Header& h = slot->header;
    if (!(h.flags & m5a::kFlagTileDelta) || frame >= h.frameCount) return false;
    outTilesX = (h.width + m5a::kTileSide - 1) / m5a::kTileSide;

    uint32_t start = 0, end = 0;
    if (!frameOffset(slot->file, frame, start, end)) return false;
    if (!slot->file.seek(start)) return false;

    uint16_t count = 0;
    if (slot->file.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) {
        return false;
    }
    if (count > maxTiles) return false;
    const size_t want = static_cast<size_t>(count) * sizeof(uint16_t);
    if (slot->file.read(reinterpret_cast<uint8_t*>(indices), want) != static_cast<int>(want)) {
        return false;
    }
    outCount = count;
    return true;
}

bool AssetPack::readClipTileData(const char* name, uint16_t frame, uint16_t first,
                                 uint16_t count, uint8_t* dst) {
    if (!dst || !count) return false;
    const String* path = namedClipPath(name);
    if (!path) return false;
    OpenFile* slot = acquire(*path);
    if (!slot) return false;

    const m5a::Header& h = slot->header;
    if (!(h.flags & m5a::kFlagTileDelta) || frame >= h.frameCount) return false;

    uint32_t start = 0, end = 0;
    if (!frameOffset(slot->file, frame, start, end)) return false;
    if (!slot->file.seek(start)) return false;

    uint16_t total = 0;
    if (slot->file.read(reinterpret_cast<uint8_t*>(&total), sizeof(total)) != sizeof(total)) {
        return false;
    }
    if (static_cast<uint32_t>(first) + count > total) return false;

    const uint32_t pixels = start + sizeof(uint16_t) +
                            static_cast<uint32_t>(total) * sizeof(uint16_t);
    if (!slot->file.seek(pixels + static_cast<uint32_t>(first) * m5a::kTileBytes)) return false;
    const size_t want = static_cast<size_t>(count) * m5a::kTileBytes;
    return slot->file.read(dst, want) == static_cast<int>(want);
}

bool AssetPack::readClipBand(const char* name, uint16_t frame, uint16_t rowStart, uint16_t rows,
                             uint8_t* dst, uint16_t& outWidth) {
    if (!dst) return false;
    const String* path = namedClipPath(name);
    if (!path) return false;
    OpenFile* slot = acquire(*path);
    if (!slot) return false;

    const m5a::Header& h = slot->header;
    outWidth = h.width;
    if (frame >= h.frameCount || rowStart + rows > h.height) return false;

    const size_t rowBytes = static_cast<size_t>(h.width) * 2;
    const uint32_t offset =
        m5a::kHeaderBytes + static_cast<uint32_t>(frame) * h.frameBytes + rowStart * rowBytes;
    if (!slot->file.seek(offset)) return false;
    return slot->file.read(dst, rowBytes * rows) == static_cast<int>(rowBytes * rows);
}
