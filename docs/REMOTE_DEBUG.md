# Measuring and reflashing the device over the network

The bug that motivated all of this is the one you cannot watch: pull the USB
cable and the M5GO sometimes simply stops - no reboot message, no log, nothing
to read, because the only thing that could have read it was the cable. Plug
the cable back in and the device works, which tells you nothing except that the
cable is part of the answer.

So the device now has a second voice. Everything below works with nothing
attached to it but a battery.

## The short version

```bash
tools/m5net.py --host 192.168.68.66 status     # a snapshot of everything
tools/m5net.py --host 192.168.68.66 log -f     # the firmware log, followed
tools/m5net.py --host 192.168.68.66 boots      # how the last eight boots ended
tools/m5net.py --host 192.168.68.66 serve      # reflash it from here
```

`--host` can be an address, `m5companion.local`, or left out entirely if
`$M5_HOST` is set. Everything is also on the device's own web page at
`http://<address>/`, which is the same data with tabs.

## What is measured, and why each thing is there

`GET /api/status` returns one object rather than a dozen endpoints, because the
point is a snapshot - two numbers read a second apart from a device that is
falling over do not describe the same device.

| field | what it answers |
|---|---|
| `boot`, `reason` | which start this is, and what `esp_reset_reason()` called it |
| `uptime_ms` | how long this one has lasted |
| `heap`, `min_heap` | free now, and the low-water mark since boot - a slow leak ends as a reboot, and this is the only warning |
| `battery`, `charging` | whether it is on the cable, which is the variable under test |
| `boost_held` | whether the IP5306 accepted the bit that stops it switching itself off. See below |
| `uplink_chunks`, `uplink_failures` | how the microphone's audio is reaching the server, or not |
| `spk_dropped`, `mic_dropped` | queue overruns in each direction; they sound nothing alike and mean different things |
| `fps`, `sd_bytes_per_sec` | what the renderer is actually managing |
| `state`, `expression` | where the state machine thinks it is |

## The log

`GET /api/log?since=N` returns everything the firmware has logged since byte
`N`, and the sequence number to ask for next time. The ring holds 4 KB; a
reader that falls behind is told so rather than silently missing lines.

Only `log_i`/`log_w`/`log_e` are captured - they go through `esp_log`, which is
where the hook is. Direct `Serial.print` from the console command handlers is
not, deliberately: that is the serial protocol talking to a host tool, not
diagnostics.

## What survived the restart

Two records, and the difference between them is the diagnosis.

**The RTC tail** (`previous` in `/api/log`, or the second panel on the Log tab)
is the last kilobyte of log, kept in RTC slow memory. A software reset, a
panic and a watchdog all leave that memory alone, so after any of those you get
to read the lines leading up to it.

A power cut does not. **So an empty tail is itself the answer**: it means the
rail went down rather than the firmware falling over.

**The boot history** (`/api/boot`, or `boots` on the serial console) is in NVS,
which survives losing power. Every minute the running device writes down how
long it has been up, what the battery was doing and its low-water heap. When it
dies, that record is the last thing it said.

```
   # started as    ran for   batt  low heap  ended
   7 poweron        14m 0s    69%       69k  WITHOUT WARNING
   6 task_wdt       12m 0s    72%       69k  on purpose
```

Read it like this:

- **`poweron` + "without warning"** - the rail went away. Not a crash. On the
  classic Core this is almost always the IP5306, below.
- **`task_wdt` or `panic` + "without warning"** - the firmware. The RTC tail
  has the lines before it.
- **`brownout`** - the battery sagged under load. Look at what was running.
- **"on purpose"** - the firmware asked for the restart: an OTA, a factory
  reset, the reboot button. Not a fault.

A minute of resolution is the compromise that keeps the flash writes down: one
record is 20 bytes, so the part sees an erase every couple of hours rather than
one a minute.

## The IP5306, which is probably your answer

The classic M5Stack Core's PMIC is a power-bank chip, and it behaves like one:
[when the load gets small enough it switches its boost converter
off](https://community.m5stack.com/topic/979/m5stack-basic-core-battery-power-on-issue-ip5306-strange-power-management).
Pull the USB and the load drops; let the screen go dark as well and it drops
further; the chip decides nothing is plugged into it and stops supplying. The
device is not crashing. It is being switched off, which is why the next start
says `poweron`.

The forum's answer to this is a hardware modification - [a diode from VBAT to
5V, or an ideal-diode part like the
MAX40200](https://forum.m5stack.com/topic/1822/no-reset-when-usb-power-removed-solved).
The firmware's answer is the chip's own keep-boost-on bit, set at boot on the
units whose IP5306 is reachable over I2C. `boost_held` on the status page says
whether it took.

It is not free: with the boost held on, the device never switches itself off,
and [a battery that would have lasted overnight lasts about ten
hours](https://github.com/M5ez/M5ez/issues/99). That is the trade. If
`boost_held` is `false` on your unit, the bit is not reachable and only the
hardware modification will do it.

## Reflashing, three ways

The partition table already had `ota_0` and `ota_1` at 4 MB each, and the image
is 1.4 MB, so there is room for two of everything.

### From the build system

```bash
pio run -e m5go_ota -t upload --upload-port 192.168.68.66
```

The device answers espota on port 3232 whenever its web console is up. This is
the development loop: change something, reflash, and the device never leaves
the shelf or the battery it is being tested on.

`--upload-port m5companion.local` works too - mDNS advertises whatever
`device_name` is set to - but an address is one fewer thing to go wrong.

### The device fetches it from you

```bash
tools/m5net.py serve
```

This serves `.pio/build/*/firmware.bin` from a throwaway HTTP server, works out
which of your addresses the device can actually reach, and asks it to fetch
from there. Then it waits for the device to come back and prints the new boot
number.

It is the only one of the three that works when you cannot reach the device but
the device can reach you - a guest VLAN, a laptop on a VPN, client isolation on
the access point.

By hand, the same thing:

```bash
cd .pio/build/m5go && python3 -m http.server 8000
curl -X POST "http://192.168.68.66/api/ota/pull?url=http://192.168.68.200:8000/firmware.bin"
```

### From a browser

The Firmware tab has a file picker. Pick `.pio/build/m5go/firmware.bin`. For a
device someone else is holding.

## Getting back from a firmware that does not work

Updating a device you cannot see is only half of it. The other half is what
happens when the thing you installed does not run.

Three mechanisms, because they fail differently. Each one covers what the one
before it cannot.

### 1. Rollback, for the boot after an update

The bootloader arduino-esp32 ships is built with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, so a freshly installed image runs once
on probation. If it does not declare itself healthy before the next restart,
the bootloader puts back the image it replaced.

The Arduino core normally throws this away: `initArduino()` declares the image
healthy before a line of application code has run, which means only a firmware
that fails inside Espressif's own startup would ever be reverted. Overriding
`verifyRollbackLater()` takes the decision back
([espressif/arduino-esp32#7423](https://github.com/espressif/arduino-esp32/issues/7423)
for why it must be `extern "C"`). Here, healthy means **sixty seconds with the
display task drawing and the audio task running**.

Deliberately not "the network is up". A firmware installed at a desk and then
carried to a room with different Wi-Fi would revert itself, and a router
rebooting overnight would be read as a broken image.

While an image is on probation the status page says so, and `m5net.py backups`
prints `on probation`.

### 2. The boot-loop guard, for a firmware that used to work

Rollback only protects the boot after an update. An image that ran for a week
and then met a character pack it cannot read is already marked valid, and will
crash on every boot for ever.

So consecutive boots that never reach health are counted, in RTC memory - which
a panic, a watchdog and a software reset all leave alone, and which a power cut
clears. That last part is deliberate: pulling the power is how a person says
"start again", and it should mean that.

The ladder, one rung per failed boot, each rung taken once per power-on:

| after | what happens |
|---|---|
| 3 boots | the other OTA slot, if it holds an image - that is the firmware this one replaced |
| 3 boots, no other slot | **safe mode** |
| 5 boots | the recovery application |

There is a second counter beside that one, in flash rather than RTC, counting
consecutive boots that *began* after a crash. The RTC count has a blind spot it
covers: it is cleared the moment a boot proves itself, so firmware that
reliably dies a minute after proving itself resets it every time and the ladder
never climbs. This device did exactly that - three watchdog resets in a row
without a single rung being taken. Reset reasons keep the flash count honest: a
boot after a power cut says `poweron` and does not count, so unplugging the
device four times does not look like a fault.

All of it verified on hardware: repeated failures moved the device to the other
slot, then into safe mode, where it stayed reachable on 112 KB of free heap
with the console up.

### 3. Safe mode, for looking at it

No character pack, no audio, no tile cache - those are the things most likely
to be what went wrong, and none of them is needed to be reachable. Wi-Fi and
the web console come up, the screen shows the address, and from there you can
restore a backup, install a new build, or press **Boot normally** once you have
fixed whatever it was.

Safe mode holds the counter rather than clearing it. Coming up reduced and
working is not evidence that the full firmware works, and clearing it there
would put the device straight back into the loop it just escaped.

```bash
tools/m5net.py normal      # clear the counter, boot fully next time
```

## A watchdog set to exactly the wrong number

Worth writing down, because it took a device, a boot history and a stress test
to find, and because the shape of the mistake is easy to repeat.

The loop task is watched by the task watchdog so that a loop which stops
turning becomes a reboot rather than a silent freeze. The default timeout is
five seconds. And `WebServer` sets its client's timeout to `HTTP_MAX_SEND_WAIT`
before writing a response - which is also five seconds (`WebServer.cpp:305`).

So any client that stops reading mid-reply - a browser tab closed, a phone
walking out of range, a script whose own timeout expired first - blocks the
write for up to five seconds, and the watchdog fires on a task that was never
stuck. On this device it presented as an intermittent `task_wdt` reset 35 to 50
seconds after boot, and only while something was polling the console.

**A threshold set at the maximum of what it is meant to tolerate is not a
threshold.** The loop's watchdog is now raised to ten seconds
(`appcfg::kLoopWatchdogSeconds`), which leaves the bounded stall room to finish
while still turning a real hang into a reboot.

Measured on hardware: the same polling that used to reset the device every 35
to 50 seconds now runs for six minutes with none, at a steady 30.3 fps and a
flat heap.

## The backups on the card

The flash holds at most two images and an update overwrites the older one, so
after two updates the version that was known to work is gone. The card does not
have that problem: it holds thirty-two gigabytes and an image is 1.4 MB.

A backup is written the first time each build proves itself. So **"known good"
means it ran**, not that somebody labelled it. The newest three are kept.

The order matters and was wrong the other way round at first. The firmware
proves itself, *then* copies a megabyte and a half of its own flash to the
card, *then* - having survived that and ten seconds more - gives up the
rollback. Cancelling the rollback first means declaring an image good and then
subjecting it to the heaviest thing the firmware ever does to itself; a crash
there left this device with no way back at all.

```
/companion/firmware/0.2.0-9f8e7d6c.bin   the images, named by version and image hash
/companion/firmware/known-good.json      which one, and how big it should be
```

```bash
tools/m5net.py backups              # what is there to fall back to
tools/m5net.py backup               # copy the running firmware now
tools/m5net.py restore              # put the known-good one back and restart
tools/m5net.py restore <file>       # or a specific one
```

The image is written under a temporary name and renamed only once the last byte
is down, because a truncated file with the right name is worse than no file: the
recovery application would find it, trust the name, and install a half image.

## The recovery application

A separate, much smaller program - 524 KB of a one-megabyte `factory` partition
- whose entire job is: mount the card, find a known-good image, write it into
the OTA slot, restart. No Wi-Fi, no audio, no pack.

It is in a `factory` partition because **nothing writes a factory partition
except esptool over USB**. OTA cannot reach it. It is the part of the device
that stays put while everything above it is being replaced.

```bash
pio run -e m5go -t upload        # the firmware, into app0
pio run -e recovery -t upload    # the recovery application, into factory
```

Flash the firmware first and the recovery application second; uploading either
also rewrites the bootloader, the partition table and otadata, so the device
comes back running app0 either way.

It is reached three ways: the ladder above hands over after five failed boots,
you ask for it (`tools/m5net.py recover`, or `recover` on the serial console),
or the bootloader falls there by itself when the OTA images fail their
checksums.

It counts its own attempts in NVS and stops after two. If the image it installs
fails badly enough to come back twice, installing it a third time is not what
will fix it - so it parks, says so on the screen, and offers button A to try
again or button C to start the firmware anyway. A firmware that then proves
itself clears the count.

### Why not put this in the bootloader

That was the obvious shape and it does not fit. This bootloader is 18,992 bytes
of a [28,672-byte
budget](https://docs.espressif.com/projects/esp-idf/en/release-v4.3/esp32/security/secure-boot-bootloader-size.html)
(0x1000 to the partition table at 0x8000) - about 9.6 KB spare, and an SD driver
plus FATFS is several times that. It could be made to fit by moving the
partition table, but PlatformIO's Arduino framework uses a **prebuilt**
bootloader from the framework package (`tools/sdk/esp32/bin/bootloader_dio_80m.elf`),
so replacing it means moving this project to the ESP-IDF build system. And the
bootloader is the one component with no rollback of its own.

The bootloader already does the half that matters most, for free: when the
image it selected fails its checksum, it tries the others. Corruption is
covered. What a custom bootloader would add is only the case of a
checksum-correct image that crashes on the way up - and that is what the ladder
above is for.

## What happens during an update

All three paths stop the display task and the audio task first - the panel and
the SD card share a bus, and a flash write wants the CPU. The screen shows a
progress bar drawn directly, and the loop task's watchdog is fed from the
progress callback, which is the only code that runs during a transfer.

On success the boot record is marked deliberate, so tomorrow's boot history
does not read the update as a power fault - and the new image starts on
probation. See the section above for what happens if it does not earn its keep.

## Turning it off

The console is a second TCP server on a part whose whole heap is smaller than a
JPEG, and it costs about 15 KB more than the pack server did - mDNS and the OTA
listener. That comes out of the tile cache, which is frames per second.

```json
{ "web_server": false }
```

in `/companion/config/device.json` removes all of it: the page, the API and the
OTA port. `min_heap` on the status page is how to decide whether you need to.

To keep the console but close the OTA port to strangers, set a password:

```json
{ "ota_password": "..." }
```

or `otapw <password>` on the serial console.

It guards three things: the espota port, and - as HTTP basic auth, username
`m5` - everything on the web API that changes the device, which is installing
firmware, restarting, and writing or deleting packs. Reading is left open; a
status page that needs a login is a status page nobody looks at.

The espota half takes effect at the next boot, because ArduinoOTA reads the
password once when it starts. The HTTP half is immediate.

For the host tool, `--password` or `$M5_PASSWORD`:

```bash
M5_PASSWORD=hunter2 tools/m5net.py serve
```

Basic auth over plain HTTP is worth having against a housemate and a stray
bookmark. It is worth nothing against anyone who can see the wire.
