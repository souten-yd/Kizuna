#pragma once

#include <Arduino.h>

namespace appdiag {

// Why each of the last few boots happened, and how the one before it ended.
//
// This exists because of a specific failure: pull the USB cable and the device
// is simply gone, and the next start reports "poweron" - which is what you
// would see if someone had switched it off. Nothing in RAM survives that, so
// nothing in RAM can answer the question. NVS does.
//
// Every minute the running device writes down how long it has been up and what
// the battery was doing. When it dies, that record is the last thing it said,
// and the next boot reads it back: "the previous boot ran 14 minutes, was on
// battery at 71%, and ended without a reason" is a power fault. "Ran 14
// minutes, ended in task_wdt" is a stuck loop. From across the room those two
// look identical; here they do not.
class BootLog {
public:
    // Opens the store, reads what the last boot left, and starts a record for
    // this one. Call after ConfigStore::begin().
    static void begin();

    // Updates this boot's record in place. Cheap to call every loop; it only
    // touches flash once a minute, and only when something has moved.
    static void heartbeat(uint32_t nowMs, uint8_t battery, bool charging);

    // Marks this boot as ended on purpose, so a restart the firmware asked for
    // is not read back as a power fault.
    static void noteCleanShutdown(const char* why);

    static uint32_t bootCount();

    // How many of the most recent boots began because the one before it
    // crashed - a panic or a watchdog, never a power cut.
    //
    // The RTC counter next door cannot see this case. It is cleared the moment
    // a boot proves itself, so firmware that reliably dies *after* proving
    // itself resets the counter every time and the ladder never climbs. This
    // one lives in flash and counts the evidence rather than the intention.
    //
    // Reset reasons are what makes it precise: a boot that follows a power cut
    // says "poweron" and does not count, so unplugging the device four times
    // in a row does not look like a fault.
    static uint8_t crashStreak();
    static String asJson();

private:
    static void flush(bool force);
};

}  // namespace appdiag
