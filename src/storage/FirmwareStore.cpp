#include "FirmwareStore.hpp"

#include <SD.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>

#include "AppConfig.hpp"
#include "display/DisplayTask.hpp"

namespace {

constexpr const char* kDir = "/companion/firmware";
constexpr const char* kIndex = "/companion/firmware/known-good.json";
// Enough to go back more than one bad update, few enough that a card full of
// nearly identical images is not what the directory is for.
constexpr uint8_t kKeep = 3;
constexpr size_t kChunk = 4096;

String basenameOf(const String& path) {
    const int slash = path.lastIndexOf('/');
    return slash < 0 ? path : path.substring(slash + 1);
}

bool ensureDir() {
    if (SD.exists(kDir)) return true;
    if (!SD.exists("/companion")) SD.mkdir("/companion");
    return SD.mkdir(kDir);
}

// Oldest first, by write time where the card reports one and by name where it
// does not - the names carry a version, so that ordering is not arbitrary.
void collect(String* names, time_t* times, uint8_t& count, uint8_t max) {
    count = 0;
    File dir = SD.open(kDir);
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f && count < max; f = dir.openNextFile()) {
        const String name = basenameOf(String(f.name()));
        if (!f.isDirectory() && name.endsWith(".bin")) {
            names[count] = name;
            times[count] = f.getLastWrite();
            ++count;
        }
        f.close();
    }
    dir.close();
    for (uint8_t i = 1; i < count; ++i) {
        for (uint8_t j = i; j > 0; --j) {
            const bool swap = times[j] < times[j - 1] ||
                              (times[j] == times[j - 1] && names[j] < names[j - 1]);
            if (!swap) break;
            String n = names[j]; names[j] = names[j - 1]; names[j - 1] = n;
            time_t t = times[j]; times[j] = times[j - 1]; times[j - 1] = t;
        }
    }
}

}  // namespace

const char* FirmwareStore::directory() { return kDir; }

String FirmwareStore::runningId() {
    // getSketchMD5 walks the image in flash and hashes exactly the bytes the
    // bootloader would load, so two builds that differ get different names and
    // the same build never gets copied twice.
    const String md5 = ESP.getSketchMD5();
    return String(M5COMPANION_VERSION) + "-" + md5.substring(0, 8);
}

size_t FirmwareStore::runningSize() { return ESP.getSketchSize(); }

bool FirmwareStore::backupRunning(DisplayTask* display) {
    const String name = runningId() + ".bin";
    const String path = String(kDir) + "/" + name;
    const size_t size = runningSize();
    if (!size) return false;

    if (!display || !display->pause(3000)) {
        log_w("firmware backup: display busy, will try again later");
        return false;
    }

    bool ok = false;
    do {
        if (!ensureDir()) {
            log_e("firmware backup: cannot create %s", kDir);
            break;
        }
        if (SD.exists(path.c_str())) {
            File existing = SD.open(path.c_str(), FILE_READ);
            const bool complete = existing && existing.size() == size;
            if (existing) existing.close();
            if (complete) {
                ok = true;
                break;   // this exact image is already on the card
            }
            SD.remove(path.c_str());
        }

        const esp_partition_t* running = esp_ota_get_running_partition();
        if (!running) break;

        // A partial file is worse than no file: the recovery application would
        // find it, trust the name and write a truncated image. So it is
        // written under a temporary name and only given the real one once the
        // last byte is down.
        const String tmp = path + ".part";
        SD.remove(tmp.c_str());
        File out = SD.open(tmp.c_str(), FILE_WRITE);
        if (!out) break;

        static uint8_t buf[kChunk];
        size_t done = 0;
        bool failed = false;
        while (done < size) {
            const size_t n = size - done < kChunk ? size - done : kChunk;
            if (esp_partition_read(running, done, buf, n) != ESP_OK ||
                out.write(buf, n) != n) {
                failed = true;
                break;
            }
            done += n;
            // A megabyte and a half of flash reads and card writes, all on the
            // loop task, which is watched. Resetting this task is not enough:
            // IDLE0 is watched too, so hand the core back between chunks or a
            // perfectly healthy backup becomes a task_wdt reset.
            esp_task_wdt_reset();
            delay(1);
        }
        out.close();
        if (failed) {
            SD.remove(tmp.c_str());
            log_e("firmware backup: write failed after %u bytes", (unsigned)done);
            break;
        }
        if (!SD.rename(tmp.c_str(), path.c_str())) {
            SD.remove(tmp.c_str());
            break;
        }
        log_i("firmware backup: %s (%u bytes)", path.c_str(), (unsigned)size);
        ok = true;
    } while (false);

    if (ok) {
        File idx = SD.open(kIndex, FILE_WRITE);
        if (idx) {
            idx.printf("{\"file\":\"%s\",\"size\":%u,\"version\":\"%s\",\"md5\":\"%s\"}\n",
                       name.c_str(), (unsigned)size, M5COMPANION_VERSION,
                       ESP.getSketchMD5().c_str());
            idx.close();
        }
        // Trim, but never the one just written and never the known-good one -
        // they are the same file here, and the loop below leaves the newest
        // kKeep alone in any case.
        String names[16];
        time_t times[16];
        uint8_t count = 0;
        collect(names, times, count, 16);
        for (uint8_t i = 0; count > kKeep && i < count - kKeep; ++i) {
            if (names[i] == name) continue;
            SD.remove((String(kDir) + "/" + names[i]).c_str());
            log_i("firmware backup: dropped %s", names[i].c_str());
        }
    }

    display->resume();
    return ok;
}

String FirmwareStore::knownGood() {
    File idx = SD.open(kIndex, FILE_READ);
    if (!idx) return String();
    const String text = idx.readString();
    idx.close();
    const int k = text.indexOf("\"file\"");
    if (k < 0) return String();
    const int a = text.indexOf('"', text.indexOf(':', k)) + 1;
    const int b = text.indexOf('"', a);
    return a > 0 && b > a ? text.substring(a, b) : String();
}

String FirmwareStore::listJson() {
    String names[16];
    time_t times[16];
    uint8_t count = 0;
    collect(names, times, count, 16);
    const String good = knownGood();

    String out = "{\"running\":\"";
    out += runningId();
    out += "\",\"running_size\":";
    out += runningSize();
    out += ",\"known_good\":\"";
    out += good;
    out += "\",\"files\":[";
    // Newest first, which is the order a person reads a list of backups in.
    for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
        File f = SD.open((String(kDir) + "/" + names[i]).c_str(), FILE_READ);
        const size_t size = f ? f.size() : 0;
        if (f) f.close();
        if (i != static_cast<int>(count) - 1) out += ',';
        out += "{\"file\":\"";
        out += names[i];
        out += "\",\"size\":";
        out += size;
        out += ",\"known_good\":";
        out += (names[i] == good) ? "true" : "false";
        out += '}';
    }
    out += "]}";
    return out;
}

bool FirmwareStore::restore(const String& file, DisplayTask* display, String& error) {
    const String name = basenameOf(file);
    if (name.isEmpty() || !name.endsWith(".bin")) {
        error = "not a firmware file";
        return false;
    }
    const String path = String(kDir) + "/" + name;

    if (!display || !display->pause(3000)) {
        error = "display busy";
        return false;
    }

    bool ok = false;
    File in = SD.open(path.c_str(), FILE_READ);
    do {
        if (!in) {
            error = "no such backup: " + name;
            break;
        }
        const size_t size = in.size();
        uint8_t magic = 0;
        if (size < 1024 || in.read(&magic, 1) != 1 || magic != 0xE9) {
            error = "that file is not an application image";
            break;
        }
        in.seek(0);

        if (!Update.begin(size)) {
            error = Update.errorString();
            break;
        }
        static uint8_t buf[kChunk];
        size_t done = 0;
        while (done < size) {
            const size_t want = size - done < kChunk ? size - done : kChunk;
            const int n = in.read(buf, want);
            if (n <= 0 || Update.write(buf, n) != static_cast<size_t>(n)) {
                error = Update.errorString();
                break;
            }
            done += n;
            esp_task_wdt_reset();
            // Update.write can occupy this core continuously just like the
            // backup path above. Let the watched idle task run as well.
            delay(1);
        }
        if (done != size) {
            Update.abort();
            if (error.isEmpty()) error = "short read from the card";
            break;
        }
        if (!Update.end(true)) {
            error = Update.errorString();
            break;
        }
        log_w("firmware restore: %s installed (%u bytes); restart to run it",
              name.c_str(), (unsigned)size);
        ok = true;
    } while (false);

    if (in) in.close();
    display->resume();
    return ok;
}
