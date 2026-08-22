#pragma once

#include <Arduino.h>
#include <FS.h>

class WebServer;
class DisplayTask;
class ConfigStore;
class OtaService;
struct DeviceConfig;

// The device's own web page: what it is doing, what it has been doing, and how
// to change the software it is doing it with.
//
// It started as pack management, because character packs change often and
// taking the card out every time is no way to live. It is now also the answer
// to a harder problem: the bugs that matter happen with the USB cable out, and
// a device with no cable had no voice at all. Now it has this.
//
//   GET  /                 the page
//   GET  /api/packs        the packs on the card, and which one is active
//   POST /api/upload       multipart file upload, target path in the query
//   POST /api/select       switch the active pack (persisted, applied at once)
//   POST /api/delete       remove a pack directory
//   GET  /api/status       one JSON object of everything measurable
//   GET  /api/log?since=N  the firmware log since byte N, as text
//   GET  /api/boot         why the last eight boots happened and how they ended
//   POST /api/ota          multipart .bin upload, installed and rebooted into
//   POST /api/ota/pull     fetch a .bin from a URL on the LAN and install it
//   POST /api/reboot       restart, recorded as deliberate
//   GET  /api/firmware     the backups on the card, and which one is known good
//   POST /api/firmware/backup   copy the running image to the card now
//   POST /api/firmware/restore  install a backup and restart into it
//   POST /api/recovery/normal   clear the boot-loop counter
//   POST /api/recovery/enter    hand over to the recovery application
//   POST /api/power/test        run the staged load test (see SerialConsole)
class DeviceWebServer {
public:
    // Fills in the status JSON. Kept as a callback so this class does not need
    // a pointer to every subsystem in the firmware just to read a number off
    // each of them.
    using StatusFn = void (*)(String& out, void* ctx);
    // The staged power test lives in the serial console, which owns the
    // handles it needs. This is how the browser reaches it without this class
    // growing a pointer to every subsystem.
    using PowerTestFn = void (*)(uint32_t seconds, void* ctx);

    // `password` guards everything that changes the device - installing
    // firmware, restarting it, writing or deleting packs. Reading is left
    // open: a status page that needs a login is a status page nobody looks at,
    // and it gives nothing away that the device does not already broadcast.
    void begin(DisplayTask* display, ConfigStore* store, DeviceConfig* config,
               OtaService* ota, const char* password, uint16_t port = 80);
    void setStatusProvider(StatusFn fn, void* ctx) {
        statusFn_ = fn;
        statusCtx_ = ctx;
    }
    void setPowerTest(PowerTestFn fn, void* ctx) {
        powerTestFn_ = fn;
        powerTestCtx_ = ctx;
    }
    void loop();
    bool running() const { return server_ != nullptr; }
    bool busy() const { return busy_; }

    // True once an upload has finished and the device should restart. The loop
    // does it rather than the request handler, so the browser gets its answer
    // before the socket dies under it.
    bool rebootPending() const { return rebootAtMs_ != 0; }

private:
    void routes();
    void handleIndex();
    void handlePacks();
    void handleUpload();
    void handleUploadData();
    void handleSelect();
    void handleDelete();
    void handleStatus();
    void handleLog();
    void handleBoot();
    void handleOta();
    void handleOtaData();
    void handleOtaPull();
    void handleReboot();
    void handleFirmware();
    void handleFirmwareBackup();
    void handleFirmwareRestore();
    void handleRecoveryNormal();
    void handleRecoveryEnter();
    void handlePowerTest();
    bool removeTree(const String& path);
    // Answers the request with a 401 and returns false when it should not
    // proceed.
    bool authorised();
    void scheduleReboot(uint32_t inMs);

    WebServer* server_ = nullptr;
    DisplayTask* display_ = nullptr;
    ConfigStore* store_ = nullptr;
    DeviceConfig* config_ = nullptr;
    OtaService* ota_ = nullptr;
    String password_;
    StatusFn statusFn_ = nullptr;
    void* statusCtx_ = nullptr;
    PowerTestFn powerTestFn_ = nullptr;
    void* powerTestCtx_ = nullptr;

    File upload_;
    String uploadPath_;
    bool uploadOk_ = false;
    String otaError_;
    bool otaOk_ = false;
    uint32_t rebootAtMs_ = 0;
    volatile bool busy_ = false;
};
