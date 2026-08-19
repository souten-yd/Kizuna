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

    DynamicJsonDocument doc(8192);
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

    // Resolve every expression, falling back to neutral so a partial pack
    // still boots instead of showing nothing.
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

    JsonObjectConst clips = doc["clips"];
    if (!clips.isNull()) bootClip_ = join(root_, clips["boot"] | "");

    // Sway is the largest sequentially-read clip, so it is the honest
    // benchmark target for the frame budget.
    benchPath_ = path_[0][static_cast<uint8_t>(Layer::Sway)];
    if (benchPath_.isEmpty()) benchPath_ = path_[0][static_cast<uint8_t>(Layer::Base)];

    ready_ = true;
    error_ = "";
    log_i("pack '%s' loaded: sway=%u viseme=%u eyes=%dx%d mouth=%dx%d", name_.c_str(),
          swayFrames_, visemeFrames_, rect_[2].w, rect_[2].h, rect_[3].w, rect_[3].h);
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
    ready_ = false;
    name_ = "";
    root_ = "";
    bootClip_ = "";
    benchPath_ = "";
}

const String& AssetPack::clipPath(Expression e, Layer l) const {
    const uint8_t idx = static_cast<uint8_t>(e) < kExpressionCount ? static_cast<uint8_t>(e) : 0;
    return path_[idx][static_cast<uint8_t>(l)];
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
    if (f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) != static_cast<int>(sizeof(h)) ||
        h.magic != m5a::kMagic || h.version != m5a::kVersion || !h.frameCount ||
        h.frameBytes != static_cast<uint32_t>(h.width) * h.height * 2) {
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
    if (!name) return false;
    if (!strcmp(name, "boot")) return !bootClip_.isEmpty();
    return false;
}

uint16_t AssetPack::clipFrameCount(const char* name) {
    if (!name || strcmp(name, "boot")) return 0;
    OpenFile* slot = acquire(bootClip_);
    return slot ? slot->header.frameCount : 0;
}

bool AssetPack::readClipBand(const char* name, uint16_t frame, uint16_t rowStart, uint16_t rows,
                             uint8_t* dst, uint16_t& outWidth) {
    if (!name || strcmp(name, "boot") || !dst) return false;
    OpenFile* slot = acquire(bootClip_);
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
