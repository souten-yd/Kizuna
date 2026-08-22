#pragma once

#include <Arduino.h>
#include <stdarg.h>

namespace appdiag {

// The log, kept in RAM, readable over the network.
//
// The device's only voice used to be the USB cable, and the failures worth
// chasing are exactly the ones that happen with the cable out: a reboot on
// battery, a task that stops, a link that drops at three in the morning. So
// every line the firmware logs is also kept here, in a ring that costs four
// kilobytes, and served over HTTP.
//
// The tail additionally lives in RTC memory, which a software reset, a panic
// and a watchdog all leave alone. That is what turns "it restarted" into "it
// restarted, and here are the last lines before it did". A power cut clears
// it - and that absence is itself an answer, because it means the rail went
// down rather than the firmware falling over.
class LogRing {
public:
    // Installs the hook. Safe to call once, early; before Wi-Fi, so that the
    // lines about Wi-Fi are in the buffer too.
    static void begin();

    // Everything from `since` onwards, and where that copy ends. Sequence
    // numbers count bytes ever written, so a reader that remembers `upto`
    // polls for the difference and never re-reads a line. Asking for a `since`
    // that has already scrolled out returns the oldest bytes still held, and
    // `from` says where they actually start - which is how a reader notices it
    // fell behind.
    static String since(uint32_t since, uint32_t& from, uint32_t& upto);

    static uint32_t seq();

    // The tail that survived the last restart, or an empty string if nothing
    // did. Valid for the life of this boot; reading it does not consume it.
    static const char* previousBootTail();

private:
    static int hook(const char* fmt, va_list ap);
};

}  // namespace appdiag
