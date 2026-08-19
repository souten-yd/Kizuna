#include "QnapStreamClient.hpp"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "AppConfig.hpp"

namespace {

constexpr uint32_t kHeaderTimeoutMs = 30000;
constexpr uint32_t kBodyTimeoutMs = 20000;
constexpr size_t kMaxLineBytes = 384;
constexpr size_t kAudioReadBytes = appcfg::kAudioBytesPerChunk;  // exactly 20 ms @ 16 kHz

bool readRawByte(WiFiClient& client, uint8_t& out, uint32_t timeoutMs) {
    const uint32_t started = millis();
    for (;;) {
        if (client.available() > 0) {
            const int v = client.read();
            if (v >= 0) {
                out = static_cast<uint8_t>(v);
                return true;
            }
        }
        if (!client.connected() && client.available() == 0) return false;
        if (millis() - started >= timeoutMs) return false;
        delay(1);
    }
}

bool readRawExact(WiFiClient& client, uint8_t* out, size_t bytes, uint32_t timeoutMs) {
    size_t done = 0;
    const uint32_t started = millis();
    while (done < bytes) {
        const int avail = client.available();
        if (avail > 0) {
            const size_t take = min(bytes - done, static_cast<size_t>(avail));
            const int got = client.read(out + done, take);
            if (got > 0) {
                done += static_cast<size_t>(got);
                continue;
            }
        }
        if (!client.connected() && client.available() == 0) return false;
        if (millis() - started >= timeoutMs) return false;
        delay(1);
    }
    return true;
}

bool readRawLine(WiFiClient& client, String& line, uint32_t timeoutMs) {
    line = "";
    line.reserve(96);
    const uint32_t started = millis();
    while (millis() - started < timeoutMs) {
        uint8_t c = 0;
        const uint32_t left = timeoutMs - (millis() - started);
        if (!readRawByte(client, c, left ? left : 1)) return false;
        if (c == '\n') return true;
        if (c == '\r') continue;
        if (line.length() >= kMaxLineBytes) return false;
        line += static_cast<char>(c);
    }
    return false;
}

String trimHeaderValue(const String& line) {
    const int colon = line.indexOf(':');
    if (colon < 0) return String();
    String value = line.substring(colon + 1);
    value.trim();
    return value;
}

String parseBoundary(const String& contentType) {
    String lower = contentType;
    lower.toLowerCase();
    const int pos = lower.indexOf("boundary=");
    if (pos < 0) return String();
    String out = contentType.substring(pos + 9);
    const int semi = out.indexOf(';');
    if (semi >= 0) out = out.substring(0, semi);
    out.trim();
    if (out.length() >= 2 && out[0] == '"' && out[out.length() - 1] == '"') {
        out = out.substring(1, out.length() - 1);
    }
    return out;
}

// Decodes the HTTP transfer layer. Go's net/http server uses chunked transfer
// encoding once Flush() is used, while each inner multipart part still carries
// its own Content-Length. Keeping the two layers separate makes the multipart
// parser small and deterministic.
class HttpBodyReader {
public:
    HttpBodyReader(WiFiClient& client, bool chunked, int64_t contentLength)
        : client_(client), chunked_(chunked), remaining_(contentLength) {}

    bool readExact(uint8_t* out, size_t bytes) {
        size_t done = 0;
        while (done < bytes) {
            const int got = readSome(out + done, bytes - done);
            if (got <= 0) return false;
            done += static_cast<size_t>(got);
        }
        return true;
    }

    bool readLine(String& line) {
        line = "";
        line.reserve(96);
        for (;;) {
            uint8_t c = 0;
            if (!readExact(&c, 1)) return false;
            if (c == '\n') return true;
            if (c == '\r') continue;
            if (line.length() >= kMaxLineBytes) return false;
            line += static_cast<char>(c);
        }
    }

    int readSome(uint8_t* out, size_t want) {
        if (!want || eof_) return 0;
        if (!chunked_) {
            if (remaining_ == 0) {
                eof_ = true;
                return 0;
            }
            size_t take = want;
            if (remaining_ > 0 && static_cast<int64_t>(take) > remaining_) {
                take = static_cast<size_t>(remaining_);
            }
            if (!readRawExact(client_, out, take, kBodyTimeoutMs)) return -1;
            if (remaining_ > 0) remaining_ -= static_cast<int64_t>(take);
            return static_cast<int>(take);
        }

        if (chunkRemaining_ == 0) {
            if (!startChunk()) return eof_ ? 0 : -1;
        }
        const size_t take = min(want, chunkRemaining_);
        if (!readRawExact(client_, out, take, kBodyTimeoutMs)) return -1;
        chunkRemaining_ -= take;
        if (chunkRemaining_ == 0) {
            uint8_t crlf[2];
            if (!readRawExact(client_, crlf, sizeof(crlf), kBodyTimeoutMs) || crlf[0] != '\r' ||
                crlf[1] != '\n') {
                return -1;
            }
        }
        return static_cast<int>(take);
    }

private:
    bool startChunk() {
        String line;
        if (!readRawLine(client_, line, kBodyTimeoutMs)) return false;
        const int semi = line.indexOf(';');
        if (semi >= 0) line = line.substring(0, semi);
        line.trim();
        if (line.isEmpty()) return false;
        char* end = nullptr;
        const unsigned long n = strtoul(line.c_str(), &end, 16);
        if (!end || *end != '\0') return false;
        if (n == 0) {
            // RFC 7230 trailers, normally empty for this server.
            for (;;) {
                if (!readRawLine(client_, line, kBodyTimeoutMs)) break;
                if (line.isEmpty()) break;
            }
            eof_ = true;
            return false;
        }
        chunkRemaining_ = static_cast<size_t>(n);
        return true;
    }

    WiFiClient& client_;
    bool chunked_ = false;
    bool eof_ = false;
    int64_t remaining_ = -1;
    size_t chunkRemaining_ = 0;
};

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool drainPart(HttpBodyReader& body, size_t bytes) {
    uint8_t scratch[128];
    while (bytes) {
        const size_t take = min(bytes, sizeof(scratch));
        if (!body.readExact(scratch, take)) return false;
        bytes -= take;
    }
    return true;
}

}  // namespace

void QnapStreamClient::begin(const DeviceConfig& cfg, EventBus* events, BinarySink sink,
                             void* sinkCtx) {
    cfg_ = cfg;
    events_ = events;
    sink_ = sink;
    sinkCtx_ = sinkCtx;
}

void QnapStreamClient::setNetworkAvailable(bool available) {
    networkAvailable_ = available;
    if (!available) {
        online_ = false;
        abortRequested_ = true;
        client_.stop();
    } else if (cfg_.usesQnapStream() && !cfg_.qnapHost.isEmpty()) {
        // Optimistic until the first request. This keeps the UI usable without
        // a permanent health socket; a failed connect immediately clears it.
        online_ = true;
    }
}

void QnapStreamClient::cancel() {
    abortRequested_ = true;
    client_.stop();
    if (phase_ == Phase::Uploading) phase_ = Phase::Idle;
    const uint32_t started = millis();
    while (phase_ == Phase::Receiving && millis() - started < 180) delay(5);
}

bool QnapStreamClient::connectAndWriteHeaders() {
    if (!networkAvailable_ || WiFi.status() != WL_CONNECTED || cfg_.qnapHost.isEmpty()) return false;
    client_.stop();
    client_.setTimeout(kBodyTimeoutMs);
    if (!client_.connect(cfg_.qnapHost.c_str(), cfg_.qnapPort)) return false;

    String path = cfg_.qnapPath;
    if (path.isEmpty()) path = "/v1/voice/chat/stream?profile=m5go";
    if (!path.startsWith("/")) path = "/" + path;

    client_.printf("POST %s HTTP/1.1\r\n", path.c_str());
    client_.printf("Host: %s:%u\r\n", cfg_.qnapHost.c_str(), cfg_.qnapPort);
    client_.printf("User-Agent: M5Companion/%s\r\n", M5COMPANION_VERSION);
    client_.print("Content-Type: application/octet-stream\r\n");
    client_.printf("X-Sample-Rate: %u\r\n", static_cast<unsigned>(appcfg::kAudioSampleRate));
    client_.print("X-Qnap-Voice-Profile: m5go\r\n");
    client_.print("Transfer-Encoding: chunked\r\n");
    client_.print("Connection: close\r\n\r\n");
    return client_.connected();
}

bool QnapStreamClient::startUtterance() {
    if (!cfg_.usesQnapStream()) return false;
    if (phase_ != Phase::Idle) {
        cancel();
        if (phase_ != Phase::Idle) return false;
    }
    abortRequested_ = false;
    lastFirstAudioMs_ = 0;
    lastReplyMs_ = 0;
    if (!connectAndWriteHeaders()) {
        online_ = false;
        if (events_) events_->post(AppEvent(AppEventType::ServerDisconnected));
        return false;
    }
    online_ = true;
    phase_ = Phase::Uploading;
    return true;
}

bool QnapStreamClient::sendChunk(const uint8_t* data, size_t bytes) {
    if (phase_ != Phase::Uploading || !client_.connected() || !data || !bytes) return false;
    char header[20];
    const int n = snprintf(header, sizeof(header), "%x\r\n", static_cast<unsigned>(bytes));
    if (n <= 0 || client_.write(reinterpret_cast<const uint8_t*>(header), n) != static_cast<size_t>(n)) {
        return false;
    }
    if (client_.write(data, bytes) != bytes) return false;
    static const uint8_t crlf[] = {'\r', '\n'};
    return client_.write(crlf, sizeof(crlf)) == sizeof(crlf);
}

bool QnapStreamClient::sendAudio(const int16_t* samples, size_t sampleCount) {
    if (!samples || !sampleCount) return false;
    const bool ok = sendChunk(reinterpret_cast<const uint8_t*>(samples), sampleCount * sizeof(int16_t));
    if (!ok) {
        online_ = false;
        client_.stop();
    }
    return ok;
}

void QnapStreamClient::finishUtterance(bool cancelled) {
    if (phase_ != Phase::Uploading) {
        if (!cancelled && events_) events_->post(AppEvent(AppEventType::ServerIdle));
        return;
    }
    if (cancelled) {
        client_.stop();
        phase_ = Phase::Idle;
        return;
    }
    static const char finalChunk[] = "0\r\n\r\n";
    if (client_.write(reinterpret_cast<const uint8_t*>(finalChunk), sizeof(finalChunk) - 1) !=
        sizeof(finalChunk) - 1) {
        fail("failed to terminate HTTP upload");
        phase_ = Phase::Idle;
        return;
    }

    responseStartMs_ = millis();
    phase_ = Phase::Receiving;
    if (events_) events_->post(AppEvent(AppEventType::ServerThinking));
    if (xTaskCreatePinnedToCore(responseTaskThunk, "qnap-rx", 6144, this, 3, &responseTask_, 0) !=
        pdPASS) {
        fail("failed to create qnap response task");
        client_.stop();
        phase_ = Phase::Idle;
    }
}

void QnapStreamClient::fail(const char* reason, bool speechStarted) {
    log_w("qnap stream: %s", reason ? reason : "unknown error");
    online_ = false;
    if (events_) {
        if (speechStarted) events_->post(AppEvent(AppEventType::SpeechEnd));
        events_->post(AppEvent(AppEventType::ServerDisconnected));
    }
}

void QnapStreamClient::receiveResponse() {
    bool speechStarted = false;
    String line;
    if (!readRawLine(client_, line, kHeaderTimeoutMs)) {
        fail("no HTTP response", false);
        return;
    }
    if (!line.startsWith("HTTP/1.1 200") && !line.startsWith("HTTP/1.0 200")) {
        log_w("qnap stream: %s", line.c_str());
        fail("HTTP status was not 200", false);
        return;
    }

    bool chunked = false;
    int64_t responseLength = -1;
    String contentType;
    for (;;) {
        if (!readRawLine(client_, line, kHeaderTimeoutMs)) {
            fail("truncated HTTP headers", false);
            return;
        }
        if (line.isEmpty()) break;
        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("content-type:")) contentType = trimHeaderValue(line);
        else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) chunked = true;
        else if (lower.startsWith("content-length:")) responseLength = trimHeaderValue(line).toInt();
    }

    const String boundary = parseBoundary(contentType);
    if (boundary.isEmpty()) {
        fail("response is not multipart/mixed", false);
        return;
    }
    const String opening = "--" + boundary;
    const String closing = opening + "--";
    HttpBodyReader body(client_, chunked, responseLength);

    uint8_t audioBuf[kAudioReadBytes];
    for (;;) {
        if (abortRequested_) return;
        if (!body.readLine(line)) {
            if (speechStarted && events_) events_->post(AppEvent(AppEventType::SpeechEnd));
            fail("multipart stream ended unexpectedly", false);
            return;
        }
        if (line.isEmpty()) continue;
        if (line == closing) break;
        if (line != opening) continue;

        String partType;
        String partContentType;
        int64_t partLength = -1;
        uint32_t declaredRate = 0;
        for (;;) {
            if (!body.readLine(line)) {
                fail("truncated multipart headers", speechStarted);
                return;
            }
            if (line.isEmpty()) break;
            String lower = line;
            lower.toLowerCase();
            if (lower.startsWith("x-qnap-part-type:")) partType = trimHeaderValue(line);
            else if (lower.startsWith("content-type:")) partContentType = trimHeaderValue(line);
            else if (lower.startsWith("content-length:")) partLength = trimHeaderValue(line).toInt();
            else if (lower.startsWith("x-qnap-sample-rate:")) declaredRate = trimHeaderValue(line).toInt();
        }
        if (partLength < 0) {
            fail("multipart part has no Content-Length", speechStarted);
            return;
        }

        if (partType == "audio" || partContentType.startsWith("audio/wav")) {
            if (partLength < 44) {
                if (!drainPart(body, static_cast<size_t>(partLength))) {
                    fail("short audio part", speechStarted);
                    return;
                }
                continue;
            }
            uint8_t wav[44];
            if (!body.readExact(wav, sizeof(wav))) {
                fail("truncated WAV header", speechStarted);
                return;
            }
            const uint16_t channels = le16(wav + 22);
            const uint32_t sampleRate = le32(wav + 24);
            const uint16_t bits = le16(wav + 34);
            if (memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4) || channels != 1 ||
                bits != 16 || sampleRate != appcfg::kAudioSampleRate ||
                (declaredRate && declaredRate != appcfg::kAudioSampleRate)) {
                log_w("qnap stream: unsupported WAV ch=%u bits=%u rate=%u declared=%u",
                      channels, bits, (unsigned)sampleRate, (unsigned)declaredRate);
                if (!drainPart(body, static_cast<size_t>(partLength - 44))) {
                    fail("could not drain unsupported audio", speechStarted);
                    return;
                }
                continue;
            }

            if (!speechStarted) {
                speechStarted = true;
                lastFirstAudioMs_ = millis() - responseStartMs_;
                if (events_) events_->post(AppEvent(AppEventType::SpeechBegin));
                log_i("qnap stream: first audio after %u ms", (unsigned)lastFirstAudioMs_);
            }

            size_t remaining = static_cast<size_t>(partLength - 44);
            while (remaining) {
                if (abortRequested_) return;
                const size_t take = min(remaining, sizeof(audioBuf));
                if (!body.readExact(audioBuf, take)) {
                    fail("truncated audio part", speechStarted);
                    return;
                }
                if (sink_) sink_(audioBuf, take, sinkCtx_);
                // One block is exactly 20 ms at 16 kHz. Pace reads at the
                // speaker rate so the fixed 480 ms playback queue becomes a
                // true bounded ring buffer and TCP backpressure reaches the
                // NAS instead of dropping chunks when Piper runs > real time.
                if (take == appcfg::kAudioBytesPerChunk) vTaskDelay(pdMS_TO_TICKS(20));
                remaining -= take;
            }
        } else {
            const size_t n = static_cast<size_t>(partLength);
            if (n < 768) {
                char json[768];
                if (!body.readExact(reinterpret_cast<uint8_t*>(json), n)) {
                    fail("truncated JSON part", speechStarted);
                    return;
                }
                json[n] = '\0';
                if (partType == "error") {
                    log_w("qnap stream error: %s", json);
                    if (events_) events_->post(AppEvent(AppEventType::FatalError));
                } else if (partType == "done") {
                    StaticJsonDocument<512> doc;
                    if (!deserializeJson(doc, json)) {
                        lastReplyMs_ = doc["timings"]["total_ms"] | (millis() - responseStartMs_);
                        const uint32_t first = doc["timings"]["first_audio_ready_ms"] | 0;
                        log_i("qnap done: server_first_audio=%u ms total=%u ms heap=%u",
                              (unsigned)first, (unsigned)lastReplyMs_, (unsigned)ESP.getFreeHeap());
                    }
                }
            } else if (!drainPart(body, n)) {
                fail("could not drain JSON part", speechStarted);
                return;
            }
        }
    }

    online_ = true;
    if (speechStarted) {
        if (events_) events_->post(AppEvent(AppEventType::SpeechEnd));
    } else if (events_) {
        events_->post(AppEvent(AppEventType::ServerIdle));
    }
}

void QnapStreamClient::responseTaskThunk(void* ctx) {
    auto* self = static_cast<QnapStreamClient*>(ctx);
    if (self) {
        self->receiveResponse();
        self->client_.stop();
        self->phase_ = Phase::Idle;
        self->responseTask_ = nullptr;
        self->abortRequested_ = false;
    }
    vTaskDelete(nullptr);
}
