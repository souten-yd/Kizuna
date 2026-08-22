#include "SerialConsole.hpp"

#include "Board.hpp"
#include "ResetReason.hpp"

#include <M5Unified.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <SD.h>

#include "AppConfig.hpp"
#include "diag/BootLog.hpp"
#include "diag/LogRing.hpp"
#include "diag/Recovery.hpp"
#include "storage/FirmwareStore.hpp"
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
                          NetworkManager* network, AudioManager* audio,
                          LedController* leds, EventBus* events, PowerManager* power) {
    display_ = display;
    configStore_ = configStore;
    config_ = config;
    network_ = network;
    audio_ = audio;
    leds_ = leds;
    events_ = events;
    power_ = power;
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
#if M5COMPANION_BOARD_CORES3
        Serial.println("err fixed USB CDC baud");
#else
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
#endif
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
    } else if (!strcmp(cmd, "led")) {
        // Isolating the LEDs from whatever else is misbehaving is a thing worth
        // doing without a rebuild, so this persists and takes effect at once.
        const char* arg = strtok(nullptr, " ");
        if (!leds_ || !config_ || !configStore_ || !arg) {
            Serial.println("err usage: led <0-255>");
            return;
        }
        const uint8_t level = static_cast<uint8_t>(strtoul(arg, nullptr, 10));
        config_->ledBrightness = level;
        configStore_->save(*config_);
        leds_->setBrightness(level);
        Serial.println("ok");
    } else if (!strcmp(cmd, "sleep")) {
        // The same event the button posts, so the screen going dark from here
        // is the same thing in every respect - including that movement will
        // not undo it.
        const char* arg = strtok(nullptr, " ");
        if (!events_ || !arg || (strcmp(arg, "on") && strcmp(arg, "off"))) {
            Serial.println("err usage: sleep on|off");
            return;
        }
        events_->post(AppEvent(strcmp(arg, "on") ? AppEventType::WakeRequested
                                                 : AppEventType::SleepRequested));
        Serial.println("ok");
    } else if (!strcmp(cmd, "debug")) {
        // The B button used to hold this; it holds mute now, which is the more
        // useful thing to have under a thumb.
        const char* arg = strtok(nullptr, " ");
        if (!display_ || !arg || (strcmp(arg, "on") && strcmp(arg, "off"))) {
            Serial.println("err usage: debug on|off");
            return;
        }
        DisplayTask::CommandMsg msg;
        msg.cmd = DisplayTask::Command::SetDebug;
        msg.value = strcmp(arg, "on") ? 0 : 1;
        display_->post(msg);
        Serial.println("ok");
    } else if (!strcmp(cmd, "boots")) {
        // The same record the web console shows, for when the cable is the
        // thing that is available. Reading it over USB right after a device
        // came back from the shelf is the fastest path there is to "it was a
        // power fault, not a crash".
        Serial.println(appdiag::BootLog::asJson());
    } else if (!strcmp(cmd, "log")) {
        uint32_t from = 0, upto = 0;
        const char* arg = strtok(nullptr, " ");
        if (arg && !strcmp(arg, "prev")) {
            Serial.println(appdiag::LogRing::previousBootTail());
        } else {
            Serial.print(appdiag::LogRing::since(arg ? strtoul(arg, nullptr, 10) : 0, from,
                                                 upto));
            Serial.printf("\n[log %u..%u]\n", (unsigned)from, (unsigned)upto);
        }
    } else if (!strcmp(cmd, "power")) {
        const char* arg = strtok(nullptr, " ");
        cmdPowerTest(arg ? strtoul(arg, nullptr, 10) : 6);
    } else if (!strcmp(cmd, "recovery")) {
        String out;
        appdiag::Recovery::appendJson(out);
        Serial.println(out);
    } else if (!strcmp(cmd, "backups")) {
        Serial.println(FirmwareStore::listJson());
    } else if (!strcmp(cmd, "backup")) {
        Serial.println(FirmwareStore::backupRunning(display_) ? "ok" : "err backup failed");
    } else if (!strcmp(cmd, "restore")) {
        // Writes the image into the slot this one is not running from and
        // selects it; the restart is separate so a mistake is still a command
        // away from being acted on.
        const char* arg = strtok(nullptr, " ");
        String file = arg ? arg : FirmwareStore::knownGood();
        if (file.isEmpty()) {
            Serial.println("err no backup named, and no known-good one on the card");
            return;
        }
        String error;
        if (FirmwareStore::restore(file, display_, error)) {
            Serial.printf("ok %s installed - `reboot` to run it\n", file.c_str());
        } else {
            Serial.printf("err %s\n", error.c_str());
        }
    } else if (!strcmp(cmd, "recover")) {
        // Hands over to the recovery application, which does the whole thing
        // by itself: card, image, install, restart.
        if (!appdiag::Recovery::factoryPresent()) {
            Serial.println("err no recovery application installed "
                           "(pio run -e recovery -t upload)");
            return;
        }
        Serial.println("ok");
        Serial.flush();
        appdiag::Recovery::bootFactory();
    } else if (!strcmp(cmd, "normal")) {
        appdiag::Recovery::clearBootLoop();
        Serial.println("ok - the next boot is a full one");
    } else if (!strcmp(cmd, "reboot")) {
        appdiag::BootLog::noteCleanShutdown("console");
        Serial.println("ok");
        Serial.flush();
        delay(100);
        ESP.restart();
    } else if (!strcmp(cmd, "otapw")) {
        // Empty clears it. One password, two doors: the espota port, and
        // basic auth on the web API's mutating half. The web half takes
        // effect at once; the espota half waits for the next boot, because
        // ArduinoOTA reads the password once when it starts.
        const char* arg = strtok(nullptr, " ");
        if (!configStore_ || !config_) {
            Serial.println("err no config");
            return;
        }
        config_->otaPassword = arg ? arg : "";
        configStore_->save(*config_);
        Serial.printf("ok %s (the espota port picks it up at the next boot)\n",
                      config_->otaPassword.isEmpty() ? "cleared" : "set");
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
        "\"reset\":\"%s\","
        "\"cache_slots\":%u,\"cache_hits\":%u,\"cache_misses\":%u,\"dropped\":%u,"
        "\"spk_dropped\":%u,\"mic_dropped\":%u,"
        "\"underruns\":%u,\"refused\":%u}\n",
        M5COMPANION_VERSION, ESP.getFreeHeap(), ESP.getMinFreeHeap(), stats.sdStatus,
        stats.packName, stats.fpsX10 / 10, stats.fpsX10 % 10, stats.sdBytesPerSec,
        stats.budgetBytesPerSec, stats.drawnBytesPerSec,
        appdiag::resetReasonName(), stats.cacheSlots,
        stats.cacheHits, stats.cacheMisses, audio_ ? audio_->droppedChunks() : 0,
        audio_ ? audio_->droppedPlayback() : 0, audio_ ? audio_->droppedCapture() : 0,
        audio_ ? audio_->playbackUnderruns() : 0,
        audio_ ? audio_->playbackRefusals() : 0);
}

// Turns the loads on one at a time and says so before each one, so that a
// device which dies partway through leaves behind the name of the thing that
// killed it.
//
// This exists because the interesting failure - the device locking up
// mid-utterance once the cable is out - has no instrument pointed at it. The
// classic Core has no battery voltage to read: the IP5306 reports five levels
// through I2C and nothing else, so a cell sagging under load looks exactly
// like a cell that is fine. What can be done instead is to apply the loads
// separately and see which one it cannot take.
//
// Every stage announces itself through log_i, which means it lands in the RTC
// tail as well as on this port. So after a lock-up: power-cycle, then
// `log prev` - or the Log tab - and the last line is the stage that did it.
void SerialConsole::cmdPowerTest(uint32_t secondsPerStage) {
    if (!leds_ || !audio_ || !network_ || !config_) {
        Serial.println("err not ready");
        return;
    }
    if (secondsPerStage < 2) secondsPerStage = 2;
    if (secondsPerStage > 30) secondsPerStage = 30;

    // What the bar is actually set to, not what the config says: on battery
    // the power policy has already switched it off, and restoring the stored
    // value at the end would quietly undo that.
    const uint8_t restoreBrightness = leds_->brightness();
    const bool wifiUp = network_->wifiConnected();

    Serial.printf("power test: %u s per stage, %s, battery %d%%\n",
                  (unsigned)secondsPerStage, wifiUp ? "wifi up" : "NO WIFI - stage 5 skipped",
                  M5.Power.getBatteryLevel());
    Serial.println("power test: if the device dies, `log prev` after the next boot "
                   "names the stage");

    WiFiUDP udp;
    // Broadcast to a port nothing is listening on. The point is the radio
    // transmitting, not anything receiving; this reproduces the uplink's shape
    // - a 640 byte frame fifty times a second - without needing a server.
    static uint8_t filler[640];
    const IPAddress broadcast(255, 255, 255, 255);
    if (wifiUp) udp.begin(50000);

    struct Stage {
        const char* name;
        bool backlight;
        bool leds;
        bool mic;
        bool radio;
    };
    static const Stage stages[] = {
        {"1 quiet - screen dim, no leds, no mic, no radio", false, false, false, false},
        {"2 + backlight at full",                            true,  false, false, false},
        {"3 + led bar at full white",                        true,  true,  false, false},
        {"4 + microphone capturing",                         true,  true,  true,  false},
        {"5 + radio sending 50 frames a second",             true,  true,  false, true},
        {"6 everything at once - this is what LISTEN does",  true,  true,  true,  true},
    };

    for (const Stage& stage : stages) {
        if (stage.radio && !wifiUp) {
            log_i("power test: skipping %s (no wifi)", stage.name);
            continue;
        }
        // Announced before the loads go on, so the last line in the log is the
        // stage that was not survived rather than the last one that was.
        log_i("power test: stage %s", stage.name);
        delay(30);

        M5.Display.setBrightness(stage.backlight ? 255 : 20);
        leds_->setBrightness(stage.leds ? 255 : 0);
        if (stage.mic) {
            audio_->requestCapture();
        } else {
            audio_->requestIdle();
        }

        const uint32_t until = millis() + secondsPerStage * 1000;
        uint32_t sent = 0;
        uint32_t nextTick = millis();
        while (static_cast<int32_t>(millis() - until) < 0) {
            if (stage.leds) leds_->update(CompanionState::Listening, 4, true, false, millis());
            if (stage.mic) {
                AudioManager::Chunk chunk;
                while (audio_->popCapture(chunk)) {}   // keep the queue moving
            }
            if (stage.radio && static_cast<int32_t>(millis() - nextTick) >= 0) {
                nextTick += 20;
                udp.beginPacket(broadcast, 50000);
                udp.write(filler, sizeof(filler));
                udp.endPacket();
                ++sent;
            }
            esp_task_wdt_reset();
            delay(1);
        }
        log_i("power test: stage survived (%u frames sent, heap %u)", (unsigned)sent,
              (unsigned)ESP.getFreeHeap());
    }

    if (wifiUp) udp.stop();
    audio_->requestIdle();
    leds_->setBrightness(restoreBrightness);
    // The stages wrote the backlight directly, behind PowerManager's back, so
    // it has to be told to stop believing its cached value - otherwise the
    // panel stays wherever the last stage left it.
    if (power_) power_->invalidate();
    log_i("power test: finished, all stages survived");
    Serial.println("ok");
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
