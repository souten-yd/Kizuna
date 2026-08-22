# Changelog

## Unreleased

### Device

- CoreS3 is now the default target. It uses the built-in ES7210 microphone at
  16 kHz, AW88298 speaker path, touch button zones and native USB CDC.
- CoreS3 gets a permanent four-control touch rail on the right edge. Its larger
  52 x 60 px targets combine volume/mute and brightness/screen-off as tap/hold
  gestures, alongside hold-to-talk and settings. The vector icons and active
  accents have been redrawn for the wider controls; holding the gear at power-on
  still opens provisioning and holding it for 3.5 seconds still resets.
- The character composition is shifted 26 px left and clipped to the 268 px
  content viewport, keeping it visually centred beside the touch rail. The rail
  is now redrawn only for a press or a relevant state change, eliminating the
  flicker caused by repainting it over every character update.
- Firmware backup and restore yield between 4 KiB flash/SD chunks. Feeding the
  loop task watchdog alone left IDLE0 starved and caused a `task_wdt` restart
  about 62 seconds after an OTA update, just as the proven image was backed up.
- M5GO-only protection workarounds are disabled on CoreS3: no IP5306 boost
  register write, battery load shedding, 150 ms uplink holdoff, ADC busy-wait
  watchdog override or GPIO15 NeoPixel traffic. OTA rollback and boot-loop safe
  mode remain because they protect firmware updates rather than the old power
  hardware.
- The loop task's watchdog is raised to ten seconds. It was five, and so is
  `HTTP_MAX_SEND_WAIT` - the timeout `WebServer` puts on its client before
  writing a response. A client that stopped reading mid-reply therefore blocked
  the loop for exactly as long as the watchdog would tolerate, and the device
  reset on a task that was never stuck. It presented as an intermittent
  `task_wdt` 35 to 50 seconds after boot, only while something was polling the
  console. Six minutes of that same polling now costs no resets at all.
- The rollback is given up *after* the backup, not before. The firmware proves
  itself, copies itself to the card, survives ten more seconds, and only then
  cancels the rollback. The other order declared an image good and then put it
  through the heaviest work it ever does - and when that killed it, there was
  no way back.
- A second boot-loop counter, in flash, counting consecutive boots that began
  after a crash. The RTC counter is cleared when a boot proves itself, so
  firmware that dies a minute *after* proving itself reset it every time and
  the escalation never climbed a rung. Reset reasons keep it honest: a boot
  after a power cut does not count.
- `log_i`/`log_w`/`log_e` from this firmware are routed through `esp_log` so the
  log ring can see them at all. arduino-esp32 sends them to `ets_printf`, which
  writes the UART registers directly and touches neither stdout nor `esp_log` -
  so reading the log over the network returned driver noise and not one line the
  firmware had written. Applied to `src/` only; a global `-DTAG` collides with
  M5GFX.
- The firmware backup now runs for images flashed over USB, not only ones
  installed by OTA. It was keyed on "the bootloader will not take this away"
  rather than "this has actually run for a minute", and a USB-flashed image is
  the former from the instant it starts.

- On battery the device now gives up transmit power (capped at 13 dBm), the LED
  bar and some backlight. The radio at full power spikes to several hundred
  milliamps fifty times a second while an utterance goes up, and a cell with age
  on it answers that with a voltage drop. `battery_saver: false` in device.json
  turns it off.
- The audio uplink waits 150 ms after the button before its first frame. The
  face changing to `listening` is a burst of SD reads and SPI writes and the
  radio starting is a burst of transmit spikes; they used to begin on the same
  press. Nothing is lost - the capture queue holds 320 ms - and the two bursts
  no longer land together.
- `listen: begin` and `listen: end` are logged with the battery reading, whether
  the cable is in, and the transmit power in force. Those lines survive in RTC
  memory, so a device that locks up mid-utterance says so after a power cycle.
- A staged power test - `m5net.py power`, or `power` on the serial console -
  turns the loads on one at a time and announces each stage before it starts.
  The classic Core has no battery voltage to read (the IP5306 reports five
  levels and nothing else), so applying the loads separately is the only way to
  find which one a tired cell cannot supply. If the device dies partway, the
  last line names the stage.

- An update that does not work now undoes itself. arduino-esp32's bootloader
  already supports rollback; the Arduino core throws it away by declaring every
  image healthy before application code runs. Overriding `verifyRollbackLater()`
  takes that decision back, and the firmware now earns it - a minute with the
  display and audio tasks both turning. Anything less and the next restart goes
  back to the image this one replaced.
- A boot-loop guard for the case rollback cannot see: an image that was fine for
  a week and then met a pack it cannot read is already marked valid. Boots that
  never reach health are counted in RTC memory, and three of them in a row moves
  to the other OTA slot, or into safe mode.
- Safe mode: Wi-Fi and the web console, no pack, no audio, no tile cache. Those
  are the things most likely to be what went wrong and none of them is needed to
  be reachable. The screen shows the address to open.
- Working firmware is copied to the SD card the first time each build proves
  itself, so "known good" means it ran rather than that somebody labelled it.
  The newest three are kept; restoring one is a button, a serial command or
  `m5net.py restore`.
- A recovery application in a new 1 MB `factory` partition - 524 KB, no Wi-Fi,
  no audio, no pack. Its whole job is to mount the card, write a known-good
  image back and restart. OTA cannot reach a factory partition, so it stays put
  while everything above it is replaced. `pio run -e recovery -t upload`.
- The partition table gains that `factory` partition, taken from the front of an
  8 MB spiffs partition this firmware has never used. app0 and app1 are
  untouched at 4 MB each, so nothing about the existing images or the size
  checks changes.

- A web console at `http://<device-ip>/`, and `tools/m5net.py` for the same
  thing from a terminal. Status, the firmware log, the boot history, pack
  management and firmware updates. On by default now rather than opt-in: it is
  the only way to see inside a device with no cable attached, and the bugs
  worth chasing are the ones that only happen with no cable attached.
- Firmware updates over Wi-Fi, three ways. `pio run -e m5go_ota -t upload`
  from the build system; `tools/m5net.py serve`, where the device fetches the
  image from a throwaway HTTP server on the host and nothing has to reach the
  device; and a file picker on the web page. The partition table already had
  two 4 MB app slots.
- The firmware log is kept in a 4 KB ring and served over HTTP, and its last
  kilobyte lives in RTC memory - which a panic, a watchdog and a software reset
  all leave alone. So a restart now comes with the lines that led to it. A
  power cut does clear it, and that absence is the diagnosis for a different
  fault.
- A boot history in NVS: why each of the last eight boots happened, how long it
  ran, what the battery was doing and its low-water heap, updated once a
  minute. It survives losing power, which is the point - a device that vanished
  on battery can be asked what it was doing when it went.
- `boots`, `log` and `otapw` on the serial console; `sleep` and `debug` too.
- One password, set as `ota_password`, guards the espota port and - as HTTP
  basic auth - everything on the web API that changes the device. Reading stays
  open.
- The B button now steps the volume through five levels and shows a bar for
  them; holding it mutes. Debug moved to the serial console, which is where the
  numbers were being read anyway.
- The loop task is watched by the task watchdog. The Arduino port subscribes
  core 0's idle task and not core 1's, so a loop that stopped turning stopped
  everything and reported nothing - which is exactly what "it freezes on LISTEN
  with the cable out" looked like.
- LISTENING is bounded at 45 seconds. It is entered by holding a button and
  left by releasing it, so it should never have been able to outlive the press,
  and it did whenever the release went missing.
- Once one audio chunk fails to send, the rest of the utterance is dropped
  rather than each one paying the library's write timeout. Fifty of those a
  second is what turned a dead socket into a device that answered its buttons
  once every few seconds.
- The IP5306's keep-boost-on bit is set at boot where the chip is reachable
  over I2C. The power-bank chip in the classic Core switches its own boost
  converter off under a light load, which is why pulling the USB could make the
  device vanish and report `poweron` on the way back. `boost_held` on the
  status page says whether the bit took.
- Sleeping on purpose is no longer undone by movement. Letting go of the button
  that turns the screen off moves the device, the accelerometer called that a
  pickup, and the screen came back in the same gesture that darkened it. Sleep
  the device fell into on its own still wakes to a hand.
- Asleep, the backlight goes off rather than dim.
- The companion protocol also runs over the USB serial link, so a board with
  no Wi-Fi credentials can reach a server through the cable that flashed it.
  `tools/usb_link.py` bridges it to an unmodified WebSocket server.
- `stat` and `put` no longer ask the SD library for a path that is not there,
  which logged an error onto the same port the console replies on.

### Tools

- `m5net.py` talks to a device over the network: `status`, `log -f`, `boots`,
  `push`, `serve`, `reboot`, `backups`, `backup`, `restore`, `recover`,
  `normal`. `serve` works out which of the host's addresses
  the device can actually reach before telling it where to fetch from, which
  is the difference between working and not on a laptop with a VPN up.
- `push_sd.py` skips firmware log lines when reading a reply. One log line
  arriving mid-dialogue used to be read as the answer to the previous command
  and every reply after it was off by one - which is what a first sync of a
  brand new pack, where every `stat` misses, reliably produced.
- `push_sd.py` finds a device that a killed run left at the transfer baud
  rate, instead of reporting it as a board with no firmware.
- `usb_link.py` restores the baud rate on the way out, for the same reason.

### Assets

- The production brief now maps the twenty-two eye parts onto the twelve slots
  the firmware addresses, requires both eyes to be in the same state outside
  the two winks, and requires the gaze parts to keep `open`'s outline and move
  only the iris. All three are things the first generic-sheet pack got wrong
  and that showed on the device.
- `validate_assets.py` fails an eye part whose two eyes differ in coverage,
  which is what an unintended wink looks like from the outside.

### Server

- `--llm openai` speaks to any OpenAI-compatible `/v1/chat/completions`
  endpoint - written for QnapAssistant on a NAS, specific to none of it.
- `--language` now also pins the reply language, because a small local model
  drifts back into English after a turn or two.
- Expression tags are stripped from a sentence before it is synthesised; a
  small model repeats them mid-reply and they were being read out loud.

## 0.2.0

First version verified on hardware.

### Device

- Layered renderer: raw RGB565 tiles streamed from the SD card, nothing
  decoded at runtime.
- `FrameBudget` measures SD throughput at boot and prices every tile at the
  read plus the write, because the LCD and the card share one SPI bus.
- `DisplayTask` is the sole owner of that bus and hands it over on request,
  which is what makes serial and web uploads safe.
- 12 drawn eye slots and 8 visemes; blink stages and lip sync at up to 30 Hz.
- Half-duplex audio with a 480 ms jitter buffer; the speaking pose is held
  until the buffer actually drains.
- IMU gaze, shake and pickup reactions, all local.
- Runs offline: Wi-Fi setup is opt-in (hold B+C at power-on) rather than a
  blocking first-run portal.
- Serial console for pushing packs over USB, and a Wi-Fi page for adding,
  deleting and switching them.

### Tools

- `pack_assets.py` builds a pack from a character recipe - which artwork means
  what is data, not code.
- `push_sd.py` syncs a pack over USB, comparing CRCs so only changed files go.
- `fake_m5go.py` exercises the server without hardware.

### Server

- Swappable STT / LLM / TTS. Claude, Ollama or an echo for the language model;
  faster-whisper for speech in; piper, espeak-ng or a test tone for speech out.
- Sentence-by-sentence synthesis so speech starts on the first sentence.
