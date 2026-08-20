#pragma once

#include <esp_system.h>

// Why the device started. Worth carrying to the server rather than only to the
// serial console, because the failures that matter happen with the cable out:
// a device that reboots on battery and not on USB is telling you something
// about power, and a watchdog is telling you something about a stuck task, and
// from across the room they look identical.

namespace appdiag {

inline const char* resetReasonName() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "poweron";
        case ESP_RST_EXT:      return "external";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";       // including an abort()
        case ESP_RST_INT_WDT:  return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT:      return "other_wdt";
        case ESP_RST_DEEPSLEEP:return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";    // the battery sagged
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

}  // namespace appdiag
