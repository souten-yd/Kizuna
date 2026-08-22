// The recovery application.
//
// A separate, much smaller program that lives in the factory partition. OTA
// cannot reach a factory partition - nothing writes one except esptool over
// USB - so this is the part of the device that stays put while everything
// above it is being replaced.
//
// Its whole job: mount the SD card, find a firmware image that is known to
// have worked, write it into the OTA slot, and restart into it. No Wi-Fi, no
// audio, no character pack, no asset cache. Those are the things most likely
// to be what went wrong, and none of them is needed to put the firmware back.
//
// It is reached three ways: the main firmware hands over after enough failed
// boots (App::escalateRecovery), someone asks for it from the console, or the
// bootloader falls here on its own when the OTA images fail their checksums.
//
// The image it installs comes from the card, written there by the main
// firmware the first time each build proved itself - sixty seconds of healthy
// running. So "known good" means it ran, not that someone labelled it.

#include <M5Unified.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <esp_ota_ops.h>

#include "AppConfig.hpp"

namespace {

constexpr const char* kDir = "/companion/firmware";
constexpr const char* kIndex = "/companion/firmware/known-good.json";
constexpr size_t kChunk = 4096;
// Two goes at the same image. If the firmware this installs fails badly enough
// to come back here twice, installing it a third time is not going to be what
// fixes it, and each attempt is another megabyte and a half written to flash.
constexpr uint32_t kMaxAttempts = 2;

Preferences prefs;
int16_t line = 0;

void say(uint16_t colour, const char* fmt, ...) {
    char text[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    Serial.println(text);
    if (line > 200) {
        M5.Display.fillRect(0, 60, appcfg::kScreenW, 180, TFT_BLACK);
        line = 60;
    }
    M5.Display.setTextColor(colour, TFT_BLACK);
    M5.Display.setCursor(12, line);
    M5.Display.print(text);
    line += 14;
}

void header() {
    M5.Display.setBrightness(180);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
    M5.Display.setCursor(12, 14);
    M5.Display.print("RECOVERY");
    M5.Display.setTextSize(1);
    line = 52;
}

void progress(size_t done, size_t total) {
    constexpr int16_t x = 12, y = 196, w = appcfg::kScreenW - 24, h = 16;
    static int lastPct = -1;
    const int pct = total ? static_cast<int>(done * 100 / total) : 0;
    if (pct == lastPct) return;
    lastPct = pct;
    M5.Display.drawRect(x, y, w, h, TFT_DARKGREY);
    M5.Display.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, TFT_ORANGE);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(x, y + h + 6);
    M5.Display.printf("%3d%%  %u / %u bytes   ", pct, (unsigned)done, (unsigned)total);
}

bool mountCard() {
    for (uint32_t freq : {appcfg::kSdFreqHz, 10000000U, 4000000U}) {
        if (SD.begin(appcfg::kSdCsPin, SPI, freq, "/sd", appcfg::kSdMaxOpenFiles)) return true;
        SD.end();
        delay(20);
    }
    return false;
}

// The file named by known-good.json, or - if that is missing or names
// something that is not there - the newest .bin in the directory. The index is
// preferred because it is the main firmware's own verdict; the fallback is
// there because a card that lost one small file should not cost you the
// images beside it.
String chooseImage() {
    File idx = SD.open(kIndex, FILE_READ);
    if (idx) {
        const String text = idx.readString();
        idx.close();
        const int k = text.indexOf("\"file\"");
        if (k >= 0) {
            const int a = text.indexOf('"', text.indexOf(':', k)) + 1;
            const int b = text.indexOf('"', a);
            if (a > 0 && b > a) {
                const String named = text.substring(a, b);
                if (SD.exists((String(kDir) + "/" + named).c_str())) return named;
                say(TFT_YELLOW, "known-good.json names %s, which is not here",
                    named.c_str());
            }
        }
    }
    String best;
    time_t bestTime = 0;
    File dir = SD.open(kDir);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String name(f.name());
            const int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (!f.isDirectory() && name.endsWith(".bin")) {
                const time_t when = f.getLastWrite();
                if (best.isEmpty() || when > bestTime || (when == bestTime && name > best)) {
                    best = name;
                    bestTime = when;
                }
            }
            f.close();
        }
        dir.close();
    }
    return best;
}

bool install(const String& name) {
    const String path = String(kDir) + "/" + name;
    File in = SD.open(path.c_str(), FILE_READ);
    if (!in) {
        say(TFT_RED, "cannot open %s", name.c_str());
        return false;
    }
    const size_t size = in.size();
    uint8_t magic = 0;
    if (size < 1024 || in.read(&magic, 1) != 1 || magic != 0xE9) {
        say(TFT_RED, "%s is not an application image", name.c_str());
        in.close();
        return false;
    }
    in.seek(0);

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    say(TFT_WHITE, "writing %u bytes to %s", (unsigned)size,
        target ? target->label : "?");

    if (!Update.begin(size)) {
        say(TFT_RED, "cannot start: %s", Update.errorString());
        in.close();
        return false;
    }
    static uint8_t buf[kChunk];
    size_t done = 0;
    bool failed = false;
    while (done < size) {
        const size_t want = size - done < kChunk ? size - done : kChunk;
        const int n = in.read(buf, want);
        if (n <= 0 || Update.write(buf, n) != static_cast<size_t>(n)) {
            failed = true;
            break;
        }
        done += n;
        progress(done, size);
    }
    in.close();
    if (failed) {
        say(TFT_RED, "failed after %u bytes: %s", (unsigned)done, Update.errorString());
        Update.abort();
        return false;
    }
    if (!Update.end(true)) {
        say(TFT_RED, "cannot finish: %s", Update.errorString());
        return false;
    }
    return true;
}

// Nothing here is worth doing twice on its own, so a failure parks rather than
// retries. A person with a cable can always do more than this program can.
void park(const char* what) {
    say(TFT_RED, "%s", what);
    say(TFT_CYAN, "Button A: try again   Button C: start the firmware anyway");
    for (;;) {
        M5.update();
        if (M5.BtnA.wasClicked()) {
            ESP.restart();
        }
        if (M5.BtnC.wasClicked()) {
            const esp_partition_t* app0 = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
            if (app0 && esp_ota_set_boot_partition(app0) == ESP_OK) {
                say(TFT_WHITE, "starting app0");
                delay(400);
                ESP.restart();
            }
        }
        delay(20);
    }
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = false;
    cfg.internal_mic = false;
    cfg.internal_imu = false;
    cfg.output_power = true;
    M5.begin(cfg);
    M5.Display.setRotation(1);
    Serial.begin(appcfg::kSerialBaud);
    delay(50);

    header();
    say(TFT_WHITE, "the firmware did not stay up; putting back a working one");

    prefs.begin("m5recov", false);
    const uint32_t attempts = prefs.getUInt("tries", 0) + 1;
    prefs.putUInt("tries", attempts);
    say(TFT_WHITE, "attempt %u", (unsigned)attempts);
    if (attempts > kMaxAttempts) {
        say(TFT_YELLOW, "this has already been tried %u times", (unsigned)(attempts - 1));
        park("Reinstalling the same image again will not fix it.");
    }

    if (!mountCard()) park("No SD card. The backups live on it.");

    const String name = chooseImage();
    if (name.isEmpty()) park("No firmware backup on the card.");
    say(TFT_WHITE, "chosen: %s", name.c_str());

    if (!install(name)) park("The image could not be installed.");

    prefs.putString("last", name);
    say(TFT_GREEN, "installed; restarting into it");
    delay(800);
    ESP.restart();
}

void loop() {}
