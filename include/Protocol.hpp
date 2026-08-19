#pragma once

#include <Arduino.h>
#include "AppConfig.hpp"
#include "AppTypes.hpp"

// WebSocket protocol v2.
//
// Text frames carry JSON control messages, binary frames carry raw PCM16.
// Binary framing is intentionally header-less: the direction is unambiguous
// because the link is half-duplex and always bracketed by listen.* on the way
// up and speech.* on the way down.
namespace protocol {

constexpr uint8_t kProtocolVersion = 2;
constexpr const char* kAudioFormat = "pcm_s16le";
constexpr uint32_t kAudioRate = appcfg::kAudioSampleRate;

Expression expressionFromString(const char* text);
const char* expressionToString(Expression e);

}  // namespace protocol
