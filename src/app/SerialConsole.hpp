#pragma once

#include <Arduino.h>

#include "device/LedController.hpp"
#include "device/PowerManager.hpp"
#include "app/EventBus.hpp"

class AudioManager;
class DisplayTask;
class ConfigStore;
class NetworkManager;
struct DeviceConfig;

// A small command console on the USB serial port.
//
// The M5GO's ESP32-D0WD has no USB peripheral - the USB socket is a CP210x
// UART bridge - so the device can never appear as a mass-storage drive. This
// is the next best thing: push files onto the SD card over the same cable that
// flashes the firmware, so iterating on artwork does not mean physically
// moving the card between the board and a reader.
//
// Protocol (line based, then raw bytes):
//   ping                        -> pong <version>
//   info                        -> one JSON line of device state
//   baud <n>                    -> ok, then the port switches
//   ls <dir>                    -> one "<name> <size>" line per entry, then end
//   stat <path>                 -> size <bytes> crc <crc32>, or missing
//   mkdir <path>                -> ok
//   rm <path>                   -> ok
//   put <path> <size> <crc32>   -> ready, then <size> bytes in blocks,
//                                  ack per block, finally ok or err <reason>
//   mictest [chunks]            -> records straight from the driver and
//                                  reports the rate it actually achieved
//   wifi                        -> stored SSID, password *length*, server
//   beep [hz] [ms] [vol]        -> ok, one tone straight from the mixer
//                                  (chunked playback bypassed - a way to
//                                  tell a bad DAC from a bad pipeline)
//   reload                      -> ok, the display task reloads the pack
//
// It also carries the companion protocol itself, so a bench device with no
// Wi-Fi credentials can still reach a server through a bridge on the host:
//   link on | off               -> ok, and the device reports itself connected
//   rx <len>                    -> <len> bytes of one protocol JSON follow
//   rxb <len>                   -> <len> bytes of PCM16 speech follow
// Outbound frames are announced the same way, "@tx <len>" or "@txb <len>"
// followed by the raw bytes, so the host can find frame boundaries in a
// stream that also carries log lines.
class SerialConsole {
public:
    void begin(DisplayTask* display, ConfigStore* configStore, DeviceConfig* config,
               NetworkManager* network, AudioManager* audio, LedController* leds,
               EventBus* events, PowerManager* power);
    void poll();

    bool busy() const { return busy_; }

    // Also reachable from the web console; see DeviceWebServer::PowerTestFn.
    void runPowerTest(uint32_t secondsPerStage) { cmdPowerTest(secondsPerStage); }

private:
    void handleLine(char* line);
    void cmdInfo();
    void cmdPowerTest(uint32_t secondsPerStage);
    void cmdLs(const char* path);
    void cmdStat(const char* path);
    void cmdMkdir(const char* path);
    void cmdRm(const char* path);
    void cmdPut(const char* path, uint32_t size, uint32_t expectedCrc);
    void cmdRx(uint32_t size, bool binary);
    bool ensureParents(const char* path);

    DisplayTask* display_ = nullptr;
    NetworkManager* network_ = nullptr;
    LedController* leds_ = nullptr;
    EventBus* events_ = nullptr;
    PowerManager* power_ = nullptr;
    AudioManager* audio_ = nullptr;
    ConfigStore* configStore_ = nullptr;
    DeviceConfig* config_ = nullptr;

    char line_[192];
    uint8_t len_ = 0;
    bool busy_ = false;
};

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);
