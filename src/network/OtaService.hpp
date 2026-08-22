#pragma once

#include <Arduino.h>

class AudioManager;
class DisplayTask;

// Reflashing the device without the cable, three ways, because the three
// answer different questions.
//
//   espota   a push from the build system - `pio run -e m5go_ota -t upload`.
//            The development loop, and the reason this exists at all: the
//            failures worth chasing happen with the USB lead out, and every
//            fix for one of those used to mean plugging back in and losing
//            the state that produced it.
//
//   pull     the device fetches a .bin from a plain HTTP server on the LAN.
//            `python3 -m http.server` in the build directory is a whole
//            deployment system. It is also the only one of the three that
//            works when the thing driving the update cannot reach the device
//            but the device can reach it.
//
//   push     a file picked in the browser, on the device's own page. For a
//            device someone else is holding.
//
// All three stop the display task and the audio task first. The panel and the
// card share a bus, the flash write wants the CPU, and a half-drawn face is a
// worse thing to look at than a progress bar.
class OtaService {
public:
    void begin(const char* hostname, const char* password, DisplayTask* display,
               AudioManager* audio);
    void loop();
    bool running() const { return running_; }
    bool busy() const { return busy_; }

    // Fetches and installs from `url`, and does not return on success: the
    // device restarts into the new image. The returned string is why it
    // failed.
    String pull(const String& url);

    // The browser path, driven a chunk at a time by the web server's upload
    // handler. `total` may be zero when the browser did not say.
    bool pushBegin(size_t total);
    bool pushWrite(const uint8_t* data, size_t len);
    String pushEnd(bool aborted);   // empty on success

    const char* lastError() const { return lastError_.c_str(); }

private:
    void quiesce(const char* what);
    void release();
    void progress(size_t done, size_t total);

    DisplayTask* display_ = nullptr;
    AudioManager* audio_ = nullptr;
    String host_;
    String lastError_;
    bool running_ = false;
    bool busy_ = false;
    bool displayPaused_ = false;
    size_t pushTotal_ = 0;
    size_t pushDone_ = 0;
    int lastPct_ = -1;
};
