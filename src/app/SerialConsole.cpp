#include "SerialConsole.hpp"

#include <M5Unified.h>
#include <SD.h>

#include "AppConfig.hpp"
#include "display/DisplayTask.hpp"
#include "audio/AudioManager.hpp"
#include "network/NetworkManager.hpp"
#include "storage/ConfigStore.hpp"

namespace {

// Blocks are acknowledged one at a time. That is the flow control: the UART
// has no RTS/CTS here, and the SD card is only intermittently faster than the
// wire, so without an ack the receive buffer overruns on the first slow write.
constexpr size_t kBlockBytes = 4096;
constexpr uint32_t kBlockTimeoutMs = 5000;

uint8_t g_block[kBlockBytes];

bool readExactly(uint8_t* dst, size_t want, uint32_t timeoutMs) {
    size_t got = 0;
    uint32_t lastProgress = millis();
    while (got < want) {
        const int n = Serial.readBytes(dst + got, want - got);
        if (n > 0) {
            got += static_cast<size_t>(n);
            lastProgress = millis();
        } else if (millis() - lastProgress > timeoutMs) {
            return false;
        }
    }
    return true;
}

}  // namespace

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    // Table-driven CRC-32/ISO-HDLC. The bitwise form was fine for the transfer
    // itself, but an incremental sync CRCs every file already on the card
    // before deciding what to send - that is the whole pack read back through
    // this function, and there it dominated.
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (uint8_t b = 0; b < 8; ++b) c = (c >> 1) ^ (0xEDB88320u & (-(c & 1)));
            table[i] = c;
        }
        ready = true;
    }

    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void SerialConsole::begin(DisplayTask* display, ConfigStore* configStore, DeviceConfig* config,
                          NetworkManager* network, AudioManager* audio) {
    display_ = display;
    configStore_ = configStore;
    config_ = config;
    network_ = network;
    audio_ = audio;
    Serial.setTimeout(50);
}

void SerialConsole::poll() {
    while (Serial.available()) {
        const int c = Serial.read();
        if (c < 0) return;
        if (c == '\r') continue;
        if (c == '\n') {
            line_[len_] = '\0';
            if (len_) handleLine(line_);
            len_ = 0;
            // One command per poll keeps the animation loop honest. Speech
            // over the link arrives as one 20 ms frame every 16 ms, though,
            // and dropping to one frame per loop iteration would starve the
            // jitter buffer, so link mode drains what is already buffered.
            if (!network_ || !network_->serialLinkActive()) return;
            continue;
        }
        if (len_ < sizeof(line_) - 1) line_[len_++] = static_cast<char>(c);
    }
}

void SerialConsole::handleLine(char* line) {
    char* cmd = strtok(line, " ");
    if (!cmd) return;

    if (!strcmp(cmd, "ping")) {
        Serial.printf("pong %s\n", M5COMPANION_VERSION);
    } else if (!strcmp(cmd, "info")) {
        cmdInfo();
    } else if (!strcmp(cmd, "baud")) {
        const char* arg = strtok(nullptr, " ");
        const uint32_t baud = arg ? strtoul(arg, nullptr, 10) : 0;
        if (baud < 9600 || baud > 2000000) {
            Serial.println("err bad baud");
            return;
        }
        Serial.println("ok");
        Serial.flush();
        delay(30);
        Serial.updateBaudRate(baud);
    } else if (!strcmp(cmd, "ls")) {
        cmdLs(strtok(nullptr, " "));
    } else if (!strcmp(cmd, "stat")) {
        cmdStat(strtok(nullptr, " "));
    } else if (!strcmp(cmd, "mkdir")) {
        cmdMkdir(strtok(nullptr, " "));
    } else if (!strcmp(cmd, "rm")) {
        cmdRm(strtok(nullptr, " "));
    } else if (!strcmp(cmd, "put")) {
        const char* path = strtok(nullptr, " ");
        const char* sizeArg = strtok(nullptr, " ");
        const char* crcArg = strtok(nullptr, " ");
        if (!path || !sizeArg) {
            Serial.println("err usage: put <path> <size> [crc32]");
            return;
        }
        cmdPut(path, strtoul(sizeArg, nullptr, 10),
               crcArg ? strtoul(crcArg, nullptr, 10) : 0);
    } else if (!strcmp(cmd, "link")) {
        const char* arg = strtok(nullptr, " ");
        if (!network_ || !arg || (strcmp(arg, "on") && strcmp(arg, "off"))) {
            Serial.println("err usage: link on|off");
            return;
        }
        Serial.println("ok");
        network_->setSerialLink(!strcmp(arg, "on"));
    } else if (!strcmp(cmd, "rx") || !strcmp(cmd, "rxb")) {
        const char* sizeArg = strtok(nullptr, " ");
        const uint32_t size = sizeArg ? strtoul(sizeArg, nullptr, 10) : 0;
        if (!size || size > kBlockBytes) {
            Serial.println("err bad length");
            return;
        }
        cmdRx(size, cmd[2] == 'b');
    } else if (!strcmp(cmd, "wifi")) {
        // Reports what is stored without printing it. A password that survived
        // a save/load round trip has the length it went in with; one that did
        // not is the difference between "wrong password" and "no password".
        if (!config_) {
            Serial.println("err no config");
            return;
        }
        Serial.printf("{\"ssid\":\"%s\",\"pass_len\":%u,\"host\":\"%s\","
                      "\"port\":%u,\"connected\":%s,\"ip\":\"%s\"}\n",
                      config_->wifiSsid.c_str(),
                      static_cast<unsigned>(config_->wifiPassword.length()),
                      config_->serverHost.c_str(), config_->serverPort,
                      (network_ && network_->wifiConnected()) ? "true" : "false",
                      network_ ? network_->ipAddress() : "");
    } else if (!strcmp(cmd, "mictest")) {
        // mictest [chunks] [over_sampling] [dma_len] [dma_count] [magnification]
        auto num = [](const char* v, unsigned dflt) {
            return v ? (unsigned)strtoul(v, nullptr, 10) : dflt;
        };
        const uint16_t n = (uint16_t)num(strtok(nullptr, " "), 50);
        const uint8_t over = (uint8_t)num(strtok(nullptr, " "), 0);
        const uint16_t dlen = (uint16_t)num(strtok(nullptr, " "), 0);
        const uint8_t dcnt = (uint8_t)num(strtok(nullptr, " "), 0);
        const uint8_t mag = (uint8_t)num(strtok(nullptr, " "), 0);
        if (!audio_) {
            Serial.println("err no audio");
            return;
        }
        audio_->selfTest(n ? n : 50, over, dlen, dcnt, mag);
    } else if (!strcmp(cmd, "micclock")) {
        const char* arg = strtok(nullptr, " ");
        if (!audio_) {
            Serial.println("err no audio");
            return;
        }
        audio_->legacyMicClockReport(arg ? (uint8_t)strtoul(arg, nullptr, 10) : 0);
    } else if (!strcmp(cmd, "beep")) {
        // One tone, generated by the mixer itself. Speech reaches the speaker
        // as fifty playRaw calls a second, so anything wrong at those seams
        // is indistinguishable from a noisy DAC until something skips them.
        const char* hzArg = strtok(nullptr, " ");
        const char* msArg = strtok(nullptr, " ");
        const char* volArg = strtok(nullptr, " ");
        const float hz = hzArg ? strtof(hzArg, nullptr) : 440.0f;
        const uint32_t ms = msArg ? strtoul(msArg, nullptr, 10) : 800;
        const uint32_t vol = volArg ? strtoul(volArg, nullptr, 10) : 40;
        M5.Speaker.setVolume(vol > 255 ? 255 : vol);
        M5.Speaker.tone(hz, ms);
        Serial.println("ok");
    } else if (!strcmp(cmd, "reload")) {
        if (display_) {
            DisplayTask::CommandMsg msg;
            msg.cmd = DisplayTask::Command::ReloadPack;
            display_->post(msg);
        }
        Serial.println("ok");
    } else {
        Serial.println("err unknown command");
    }
}

void SerialConsole::cmdInfo() {
    DisplayTask::Stats stats{};
    if (display_) display_->snapshot(stats);
    Serial.printf(
        "{\"fw\":\"%s\",\"heap\":%u,\"min_heap\":%u,\"sd\":\"%s\",\"pack\":\"%s\","
        "\"fps\":%u.%u,\"sd_bps\":%u,\"budget_bps\":%u,\"drawn_bps\":%u,"
        "\"cache_slots\":%u,\"cache_hits\":%u,\"cache_misses\":%u,\"dropped\":%u,"
        "\"underruns\":%u,\"refused\":%u}\n",
        M5COMPANION_VERSION, ESP.getFreeHeap(), ESP.getMinFreeHeap(), stats.sdStatus,
        stats.packName, stats.fpsX10 / 10, stats.fpsX10 % 10, stats.sdBytesPerSec,
        stats.budgetBytesPerSec, stats.drawnBytesPerSec, stats.cacheSlots,
        stats.cacheHits, stats.cacheMisses, audio_ ? audio_->droppedChunks() : 0,
        audio_ ? audio_->playbackUnderruns() : 0,
        audio_ ? audio_->playbackRefusals() : 0);
}

void SerialConsole::cmdLs(const char* path) {
    if (!path) path = appcfg::kAssetRoot;
    if (!display_ || !display_->pause()) {
        Serial.println("err display busy");
        return;
    }
    File dir = SD.open(path);
    if (!dir) {
        Serial.println("err no such directory");
    } else {
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            Serial.printf("%s %s %u\n", entry.isDirectory() ? "d" : "f", entry.name(),
                          static_cast<unsigned>(entry.size()));
            entry.close();
        }
        dir.close();
        Serial.println("end");
    }
    display_->resume();
}

void SerialConsole::cmdStat(const char* path) {
    if (!path) {
        Serial.println("err usage: stat <path>");
        return;
    }
    if (!display_ || !display_->pause()) {
        Serial.println("err display busy");
        return;
    }
    // Checked before opening: SD.open logs an error for a path that is not
    // there, and a sync of a pack the card has never seen asks about every
    // file in it. That log goes to the same port as this reply.
    File f = SD.exists(path) ? SD.open(path, FILE_READ) : File();
    if (!f) {
        Serial.println("missing");
    } else {
        uint32_t crc = 0;
        size_t total = 0;
        while (true) {
            const int n = f.read(g_block, sizeof(g_block));
            if (n <= 0) break;
            crc = crc32Update(crc, g_block, static_cast<size_t>(n));
            total += static_cast<size_t>(n);
        }
        f.close();
        Serial.printf("size %u crc %u\n", static_cast<unsigned>(total),
                      static_cast<unsigned>(crc));
    }
    display_->resume();
}

bool SerialConsole::ensureParents(const char* path) {
    char buf[128];
    strlcpy(buf, path, sizeof(buf));
    for (char* p = buf + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (!SD.exists(buf)) SD.mkdir(buf);
        *p = '/';
    }
    return true;
}

void SerialConsole::cmdMkdir(const char* path) {
    if (!path) {
        Serial.println("err usage: mkdir <path>");
        return;
    }
    if (!display_ || !display_->pause()) {
        Serial.println("err display busy");
        return;
    }
    ensureParents(path);
    const bool ok = SD.exists(path) || SD.mkdir(path);
    Serial.println(ok ? "ok" : "err mkdir failed");
    display_->resume();
}

void SerialConsole::cmdRm(const char* path) {
    if (!path) {
        Serial.println("err usage: rm <path>");
        return;
    }
    if (!display_ || !display_->pause()) {
        Serial.println("err display busy");
        return;
    }
    Serial.println(SD.remove(path) ? "ok" : "err remove failed");
    display_->resume();
}

void SerialConsole::cmdPut(const char* path, uint32_t size, uint32_t expectedCrc) {
    if (!display_ || !display_->pause()) {
        Serial.println("err display busy");
        return;
    }
    busy_ = true;

    ensureParents(path);
    // Guarded for the same reason as stat: removing a path that is not there
    // logs an error onto the port this dialogue runs over.
    if (SD.exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.println("err cannot open for write");
        busy_ = false;
        display_->resume();
        return;
    }

    Serial.println("ready");

    uint32_t crc = 0;
    uint32_t remaining = size;
    bool ok = true;
    while (remaining) {
        const size_t want = remaining < kBlockBytes ? remaining : kBlockBytes;
        if (!readExactly(g_block, want, kBlockTimeoutMs)) {
            ok = false;
            Serial.println("err timeout");
            break;
        }
        if (f.write(g_block, want) != want) {
            ok = false;
            Serial.println("err write failed");
            break;
        }
        crc = crc32Update(crc, g_block, want);
        remaining -= want;
        Serial.printf("ack %u\n", static_cast<unsigned>(size - remaining));
    }
    f.close();

    if (ok) {
        if (expectedCrc && crc != expectedCrc) {
            SD.remove(path);
            Serial.printf("err crc mismatch %u != %u\n", static_cast<unsigned>(crc),
                          static_cast<unsigned>(expectedCrc));
        } else {
            Serial.printf("ok %u\n", static_cast<unsigned>(crc));
        }
    }

    busy_ = false;
    display_->resume();
}


void SerialConsole::cmdRx(uint32_t size, bool binary) {
    busy_ = true;
    const bool ok = readExactly(g_block, size, kBlockTimeoutMs);
    busy_ = false;
    if (!ok) {
        Serial.println("err timeout");
        return;
    }
    if (!network_) return;
    if (binary) {
        network_->deliverBinary(g_block, size);
    } else {
        network_->deliverText(g_block, size);
    }
}
