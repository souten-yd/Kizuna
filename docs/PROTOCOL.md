# WebSocket protocol v2

One connection, `ws://<host>:<port>/m5companion`. Text frames carry JSON
control messages; binary frames carry raw PCM. Binary framing is header-less
because the direction is never ambiguous: the link is half-duplex and every
audio burst is bracketed by `listen.*` going up and `speech.*` coming down.

Audio is 16 kHz, signed 16-bit, mono, little-endian, 320 samples (20 ms) per
frame, in both directions. Nothing resamples anywhere in the path.

## Device to server

On connect:

```json
{"type":"hello","device":"m5go","name":"M5GO-Companion","protocol":2,
 "fw":"0.2.0","audio_format":"pcm_s16le","audio_rate":16000,
 "chunk_samples":320,"ip":"192.168.1.42"}
```

Push-to-talk:

```json
{"type":"listen.begin","format":"pcm_s16le","rate":16000}
```
… binary frames of PCM16 …
```json
{"type":"listen.end"}
{"type":"listen.end","cancelled":true}
```

Status, sent on change and every 15 s:

```json
{"type":"device.state","state":"LISTEN","expression":"listening"}
{"type":"device.telemetry","battery":87,"charging":false,"heap":94016,
 "fps":30.3,"rssi":-52}
```

## Server to device

Set the state, the expression, or both:

```json
{"type":"state","state":"thinking","expression":"thinking"}
{"type":"expression","name":"happy","duration_ms":2000}
{"type":"gaze","x":-3,"y":0}
```

`state` accepts `thinking` and `idle`. `expression` accepts the ten names the
firmware knows: `neutral happy excited thinking listening speaking confused
sleepy playful error`. An expression is a transient reaction layered over the
state's resting face and reverts after `duration_ms`.

Speech:

```json
{"type":"speech.begin","format":"pcm_s16le","rate":16000}
```
… binary frames of PCM16 …
```json
{"type":"speech.end"}
```

The device buffers four chunks (~80 ms) before the speaker opens, and holds
the SPEAKING pose until the buffer has actually drained - otherwise the mouth
stops before the voice does. Pace the sending near real time; frames larger
than 20 ms are split rather than truncated, but a burst faster than playback
will eventually overflow the 24-chunk queue.

`{"type":"error"}` puts the device into its error state, which recovers on
its own once the link is healthy again.

## What the device does on its own

Losing the connection is not a failure mode that needs handling by the
server. The device keeps blinking, tracking tilt with its gaze, reacting to
being picked up, and answering its buttons; the status bar changes to
OFFLINE and the face flashes `confused` once. Reconnection is automatic.

## The same protocol over USB

Wi-Fi is the transport the product ships with. On a bench it is also the
slowest step in every debug loop: a board that has just been flashed has no
credentials, and the cable that flashed it is still attached.

So the firmware carries these same frames over that cable. `link on` in the
serial console (see `SerialConsole.hpp`) makes the device report itself
connected and start emitting its outbound messages there:

```text
@tx  <len>\n<json bytes>     one protocol message, device to host
@txb <len>\n<pcm bytes>      microphone audio, device to host
```

and accept the server's messages the same way:

```text
rx  <len>\n<json bytes>      one protocol message, host to device
rxb <len>\n<pcm bytes>       speech, host to device
```

Lengths are prefixed rather than delimited because the port also carries the
firmware's log lines, and a host needs to find frame boundaries without
scanning for a terminator that could occur inside PCM.

`tools/usb_link.py` relays that to a normal WebSocket server, which therefore
needs no changes and cannot tell the difference:

```bash
python tools/usb_link.py --url ws://127.0.0.1:8765/m5companion
```

Speech is 32 kB/s in each direction, so the link switches to 921600 baud
before it starts; 115200 would not carry one direction, let alone two. Both
transports can be live at once - the device sends to whichever is up.
