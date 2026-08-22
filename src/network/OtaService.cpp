#include "OtaService.hpp"

#include <ArduinoOTA.h>
#include <HTTPUpdate.h>
#include <M5Unified.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "audio/AudioManager.hpp"
#include "diag/BootLog.hpp"
#include "display/DisplayTask.hpp"

namespace {
OtaService* g_instance = nullptr;
constexpr uint16_t kOtaPort = 3232;   // what espota and the Arduino IDE expect
}  // namespace

void OtaService::begin(const char* hostname, const char* password, DisplayTask* display,
                       AudioManager* audio) {
    if (running_) return;
    display_ = display;
    audio_ = audio;
    host_ = hostname && *hostname ? hostname : "m5companion";
    g_instance = this;

    ArduinoOTA.setHostname(host_.c_str());
    ArduinoOTA.setPort(kOtaPort);
    // A password only if one was set. An unprotected OTA port on a home LAN is
    // a considered trade rather than an oversight - but it is the user's trade
    // to make, so `ota_password` in device.json takes it away.
    if (password && *password) ArduinoOTA.setPassword(password);
    ArduinoOTA.setMdnsEnabled(true);

    ArduinoOTA.onStart([] { g_instance->quiesce("OTA"); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        g_instance->progress(done, total);
    });
    ArduinoOTA.onEnd([] {
        appdiag::BootLog::noteCleanShutdown("ota");
        M5.Display.setBrightness(180);
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Display.setCursor(20, 100);
        M5.Display.print("OTA done - restarting");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        g_instance->lastError_ = String("espota error ") + static_cast<int>(error);
        log_e("%s", g_instance->lastError_.c_str());
        g_instance->release();
    });

    ArduinoOTA.begin();
    running_ = true;
    log_i("ota: espota on %s.local:%u (%s), password %s", host_.c_str(), kOtaPort,
          WiFi.localIP().toString().c_str(), password && *password ? "set" : "none");
}

void OtaService::loop() {
    if (running_) ArduinoOTA.handle();
}

void OtaService::quiesce(const char* what) {
    busy_ = true;
    lastPct_ = -1;
    lastError_ = "";
    if (audio_) audio_->requestIdle();
    // Best effort: a display task that will not stop is not a reason to refuse
    // the update, it is a reason to stop drawing over it.
    if (display_) displayPaused_ = display_->pause(3000);
    M5.Display.setBrightness(180);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(20, 70);
    M5.Display.print(what);
    M5.Display.setTextSize(1);
    log_i("ota: %s starting", what);
}

void OtaService::release() {
    busy_ = false;
    if (displayPaused_ && display_) display_->resume();
    displayPaused_ = false;
}

void OtaService::progress(size_t done, size_t total) {
    // The loop task is watched now, and a flash write of a megabyte takes far
    // longer than the watchdog's patience. This is the only place that runs
    // during a transfer, so it is the only place that can say the task is
    // still alive.
    esp_task_wdt_reset();

    const int pct = total ? static_cast<int>(done * 100 / total) : -1;
    if (pct == lastPct_) return;
    lastPct_ = pct;

    constexpr int16_t x = 30, y = 130, w = 260, h = 18;
    M5.Display.drawRect(x, y, w, h, TFT_DARKGREY);
    if (pct >= 0) M5.Display.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, TFT_ORANGE);
    M5.Display.setCursor(x, y + h + 8);
    M5.Display.printf("%4d%%  %u / %u bytes   ", pct, (unsigned)done, (unsigned)total);
}

String OtaService::pull(const String& url) {
    if (busy_) return "already updating";
    if (WiFi.status() != WL_CONNECTED) return "no wifi";
    if (!url.startsWith("http://") && !url.startsWith("https://")) return "url must be http";

    quiesce("Fetching update");
    M5.Display.setCursor(20, 100);
    M5.Display.print(url.c_str());

    WiFiClient client;
    httpUpdate.rebootOnUpdate(false);   // the restart is ours, after the log line
    httpUpdate.onProgress([](int done, int total) {
        g_instance->progress(static_cast<size_t>(done), static_cast<size_t>(total));
    });

    const t_httpUpdate_return result = httpUpdate.update(client, url);
    switch (result) {
        case HTTP_UPDATE_OK:
            appdiag::BootLog::noteCleanShutdown("ota-pull");
            log_i("ota: pulled %s, restarting", url.c_str());
            delay(300);
            ESP.restart();
            return "";
        case HTTP_UPDATE_NO_UPDATES:
            lastError_ = "server had no update";
            break;
        default:
            lastError_ = String(httpUpdate.getLastErrorString()) + " (" +
                         httpUpdate.getLastError() + ")";
            break;
    }
    log_e("ota: pull failed: %s", lastError_.c_str());
    release();
    return lastError_;
}

bool OtaService::pushBegin(size_t total) {
    if (busy_) {
        lastError_ = "already updating";
        return false;
    }
    quiesce("Uploading update");
    pushTotal_ = total;
    pushDone_ = 0;
    // UPDATE_SIZE_UNKNOWN lets the whole free OTA partition be the limit when
    // the browser did not send a length, which is the usual case.
    if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN)) {
        lastError_ = Update.errorString();
        log_e("ota: begin failed: %s", lastError_.c_str());
        release();
        return false;
    }
    return true;
}

bool OtaService::pushWrite(const uint8_t* data, size_t len) {
    if (!busy_) return false;
    if (Update.write(const_cast<uint8_t*>(data), len) != len) {
        lastError_ = Update.errorString();
        return false;
    }
    pushDone_ += len;
    progress(pushDone_, pushTotal_ ? pushTotal_ : pushDone_);
    return true;
}

String OtaService::pushEnd(bool aborted) {
    if (!busy_) return "not updating";
    if (aborted) {
        Update.abort();
        lastError_ = "upload aborted";
        release();
        return lastError_;
    }
    if (!Update.end(true)) {
        lastError_ = Update.errorString();
        log_e("ota: end failed: %s", lastError_.c_str());
        release();
        return lastError_;
    }
    appdiag::BootLog::noteCleanShutdown("ota-push");
    log_i("ota: %u bytes installed, restarting", (unsigned)pushDone_);
    return "";
}
