#pragma once

#include <Arduino.h>
#include <FS.h>

class WebServer;
class DisplayTask;
class ConfigStore;
struct DeviceConfig;

// Browser-based pack management, served from the device over Wi-Fi.
//
// Character packs are meant to change often, so there has to be a way to add,
// remove and switch them that does not involve taking the card out. This is
// that way; the serial console is the same job over USB for when there is no
// network yet.
//
//   GET  /            an upload and management page
//   GET  /api/packs   the packs on the card, and which one is active
//   POST /api/upload  multipart file upload, target path in the query string
//   POST /api/select  switch the active pack (persisted, applied immediately)
//   POST /api/delete  remove a pack directory
class PackWebServer {
public:
    void begin(DisplayTask* display, ConfigStore* store, DeviceConfig* config,
               uint16_t port = 80);
    void loop();
    bool running() const { return server_ != nullptr; }
    bool busy() const { return busy_; }

private:
    void routes();
    void handleIndex();
    void handlePacks();
    void handleUpload();
    void handleUploadData();
    void handleSelect();
    void handleDelete();
    bool removeTree(const String& path);

    WebServer* server_ = nullptr;
    DisplayTask* display_ = nullptr;
    ConfigStore* store_ = nullptr;
    DeviceConfig* config_ = nullptr;

    File upload_;
    String uploadPath_;
    bool uploadOk_ = false;
    volatile bool busy_ = false;
};
