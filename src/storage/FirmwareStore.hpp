#pragma once

#include <Arduino.h>

class DisplayTask;

// Copies of firmware that works, kept on the SD card.
//
// The flash holds at most two images and an update overwrites the older one,
// so after two updates the version that was known to work is gone. The card
// does not have that problem: it holds thirty-two gigabytes and a firmware
// image is one and a half megabytes.
//
// A backup is taken the first time an image proves itself - the same moment
// the rollback is confirmed, sixty seconds of healthy running in - so what is
// on the card is never a copy of something that has not run. Which means the
// answer to "put back the last one that worked" is a file, and the thing that
// reads it does not have to be this firmware: the recovery application in the
// factory partition reads the same directory with the same rules.
//
//   /companion/firmware/<version>-<md5 prefix>.bin   the images
//   /companion/firmware/known-good.json              which one, and its size
class FirmwareStore {
public:
    // The running image's own identity, from the image header rather than from
    // anything the build wrote down.
    static String runningId();
    static size_t runningSize();

    // Writes the running image to the card if that exact image is not already
    // there, then trims the directory to the newest few. Pauses the display
    // task for the duration - the panel and the card share a bus. Returns true
    // when the card ends up holding this image, including when it already did.
    static bool backupRunning(DisplayTask* display);

    // What is on the card, as JSON, newest first.
    static String listJson();

    // The file named by known-good.json, or empty.
    static String knownGood();

    // Writes `file` into the other OTA slot and selects it. Does not restart;
    // the caller decides when. `error` is set on failure.
    static bool restore(const String& file, DisplayTask* display, String& error);

    static const char* directory();
};
