# Changelog

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
