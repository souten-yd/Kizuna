# Changelog

## Unreleased

### Device

- The companion protocol also runs over the USB serial link, so a board with
  no Wi-Fi credentials can reach a server through the cable that flashed it.
  `tools/usb_link.py` bridges it to an unmodified WebSocket server.
- `stat` and `put` no longer ask the SD library for a path that is not there,
  which logged an error onto the same port the console replies on.

### Tools

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
