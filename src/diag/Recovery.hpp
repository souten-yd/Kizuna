#pragma once

#include <Arduino.h>

namespace appdiag {

// Getting back from a firmware that does not work.
//
// Three mechanisms, because they fail differently.
//
// 1. Rollback. The bootloader that ships with arduino-esp32 is built with
//    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, so a freshly installed image runs
//    once on probation: if it does not declare itself healthy before the next
//    restart, the bootloader goes back to the image it replaced. The Arduino
//    core normally throws this away by declaring the image healthy in
//    initArduino(), before a line of application code has run - overriding
//    verifyRollbackLater() is what takes the decision back. See Recovery.cpp.
//
// 2. A boot-loop guard. Rollback only protects the boot after an update. An
//    image that was fine for a week and then met a character pack it cannot
//    read is already marked valid, and will crash on every boot for ever. So
//    consecutive boots that never reach health are counted, and after three
//    the device comes up in safe mode instead.
//
// 3. Safe mode itself: Wi-Fi, the web console and OTA, and nothing else. No
//    pack, no audio, no SD reads beyond the config. Whatever is wrong, the
//    device is reachable, and reachable is what lets you fix it.
//
// The counter lives in RTC memory, which a panic, a watchdog and a software
// reset all leave alone - and which a power cut clears. That is the right
// behaviour rather than a limitation: pulling the power is how a person says
// "start again", and it should mean that.
class Recovery {
public:
    // Before anything else can crash. Reads the running image's state, counts
    // the boot, and decides whether this one is a safe one.
    //
    // `crashStreak` is BootLog's count of consecutive boots that began after a
    // crash. It is the second opinion, and it exists because the RTC counter
    // has a blind spot: that one is cleared the moment a boot proves itself,
    // so firmware that reliably dies a minute *after* proving itself resets it
    // every time and the ladder never climbs. Ask both.
    static void begin(uint8_t crashStreak);

    // Reduced boot: the display shows a notice, the pack and the audio tasks
    // are not started, the network is.
    static bool safeMode() { return safeMode_; }

    // The running image is on probation - installed by OTA and not yet
    // confirmed. A restart now goes back to the previous image.
    static bool onTrial();

    // The bootloader will not take this image away again. True from the first
    // instant for an image flashed over USB, which never went on probation.
    static bool confirmed() { return confirmed_; }

    // This boot has actually run healthy for the probation period. Not the
    // same thing as confirmed(), and the difference matters: a USB-flashed
    // image is confirmed before setup() returns, so anything that wants
    // evidence the firmware works - taking a backup of it, say - has to ask
    // this instead. Otherwise it only ever fires for images that arrived by
    // OTA, which is exactly backwards.
    static bool proven() { return proven_; }

    // Call every loop. `healthy` is the application's own verdict; when it has
    // held for the probation period, proven() becomes true.
    //
    // It deliberately does not confirm the image. That is a separate call the
    // application makes when it has finished everything that could still go
    // wrong - see confirm().
    static void update(uint32_t nowMs, bool healthy);

    // Gives up the rollback: this image is now the one the bootloader keeps.
    //
    // Split from update() because the order was wrong the other way round. The
    // firmware's first act on proving itself is to copy a megabyte and a half
    // of flash to the SD card, and cancelling the rollback before doing that
    // means declaring an image good and then subjecting it to the one piece of
    // work most likely to show that it is not. A crash there used to leave the
    // device with no way back at all. Now the image has to survive its own
    // maintenance first.
    static void confirm();

    // Leaves safe mode deliberately: clears the counter so the next boot is a
    // normal one. For when you have fixed the thing that was wrong.
    static void clearBootLoop();

    static uint8_t bootStreak() { return streak_; }

    // Each rung of the escalation is taken once per power-on, so a device
    // cannot spend the afternoon bouncing between two images that are both
    // broken. Cleared with the counter, which means by pulling the power.
    static bool triedOtherSlot();
    static void noteTriedOtherSlot();
    static bool triedFactory();
    static void noteTriedFactory();
    static const char* runningLabel();
    static const char* otherLabel();

    // True when the other OTA slot starts with an image header. Not proof it
    // works - nothing short of booting it is - but enough to know there is
    // something there to go back to.
    static bool otherSlotBootable();

    // Points the bootloader at the other slot and restarts. Returns only on
    // failure.
    static bool bootOtherSlot();

    // The small recovery application in the factory partition, which OTA
    // cannot reach and only esptool over USB ever writes. Present on a device
    // that has had one flashed to it, absent on one that has not.
    static bool factoryPresent();

    // Points the bootloader at it without restarting, for a caller that has
    // its own reason to choose the moment - answering a browser, say.
    static bool selectFactory();

    // Starts it. It reads a known-good image off the SD card, writes it back
    // and restarts; see src/recovery/. Returns only on failure.
    static bool bootFactory();

    static void appendJson(String& out);

private:
    static bool safeMode_;
    static bool confirmed_;
    static bool proven_;
    static uint8_t streak_;
    static uint32_t healthySinceMs_;
};

}  // namespace appdiag
