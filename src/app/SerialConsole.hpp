#pragma once

#include <Arduino.h>

class DisplayTask;
class ConfigStore;
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
//   reload                      -> ok, the display task reloads the pack
class SerialConsole {
public:
    void begin(DisplayTask* display, ConfigStore* configStore, DeviceConfig* config);
    void poll();

    bool busy() const { return busy_; }

private:
    void handleLine(char* line);
    void cmdInfo();
    void cmdLs(const char* path);
    void cmdStat(const char* path);
    void cmdMkdir(const char* path);
    void cmdRm(const char* path);
    void cmdPut(const char* path, uint32_t size, uint32_t expectedCrc);
    bool ensureParents(const char* path);

    DisplayTask* display_ = nullptr;
    ConfigStore* configStore_ = nullptr;
    DeviceConfig* config_ = nullptr;

    char line_[192];
    uint8_t len_ = 0;
    bool busy_ = false;
};

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);
