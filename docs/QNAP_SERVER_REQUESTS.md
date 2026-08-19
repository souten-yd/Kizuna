# Changes wanted from QnapAssistant

Written after taking the M5GO through the whole loop - microphone, SenseVoice,
Qwen3, Piper, speaker - against a live NAS. Everything here is something that
was measured on that run, ordered by what it costs the device.

Client side for reference: an M5Stack M5GO. ESP32-D0WD, no PSRAM, **78 KB of
free heap**, a 16 kHz mono audio path, and an LCD and SD card sharing one SPI
bus. It is the constraint that shapes all of this.

---

## 1. Let `/v1/audio/speech` return 16 kHz

**The single most useful change.** Piper emits 22050 Hz and the parameter to
ask for anything else does not exist - `sample_rate`, `rate` and `sr` were all
accepted and all ignored, the response staying `X-Qnap-Sample-Rate: 22050`.

The device plays at 16 kHz, so something has to convert. Doing it in the
client cost an anti-aliasing filter that took a morning to find the need for:
interpolating 22050 down to 16000 without one folds everything above 8 kHz -
most of every "s" and "sh" - back into the band as inharmonic noise. It is
audible as grit over the whole reply.

```
POST /v1/audio/speech
{"text": "...", "lang": "ja", "backend": "piper_plus", "sample_rate": 16000}
```

Resampling once, on a NAS with a filter, beats every client doing it badly.
If Piper cannot be asked for 16 kHz directly, resampling server-side before
the response is still the right place for it.

## 2. Say how loud the audio is, or offer to normalise it

Piper's output peaks at **99% of full scale** and sits at 14% RMS. The
M5Stack Core multiplies the mixer output by eight before its DAC, on top of
the volume setting, so a reply at that level is driven hard into the limit -
audible as a chirping warble that disappears when the same file is played
quieter.

Either is fine:

- a `peak` parameter (0..1) that normalises before returning, or
- an `X-Qnap-Peak` response header, so a client can scale without scanning
  the whole buffer first.

The second is cheaper for you and enough for us.

## 3. Stream the audio instead of finishing it first

`/v1/audio/speech` currently returns `Transfer-Encoding: chunked` but only
after the whole utterance is synthesised - measured 2650 ms of TTS before the
first byte. A companion is judged on how fast it starts talking, so today the
client asks for one sentence at a time to get around this, which costs a
request per sentence.

If the response streamed as it synthesised, a client could start playing on
the first chunk and the sentence splitting could go away.

## 4. `/v1/voice/chat` cannot be used from a device

It returns the audio base64 encoded inside JSON. For a 7.7 second reply that
is **455 KB of JSON around 333 KB of WAV**, against 78 KB of free heap. There
is no way to hold it, and streaming a base64 decode out of a JSON parser on
an ESP32 is not a reasonable thing to ask of a client.

The three-endpoint path works and is what is used instead, so this is not
blocking. But if the one-call endpoint is meant for small devices, it needs a
form that is not base64-in-JSON: `multipart/mixed` with the metadata first and
the WAV as a second part, or the metadata in response headers with `audio/wav`
as the body.

## 5. An option to keep emoji out of the reply

Qwen3 ends a friendly reply with one - "こんにちは！いい天気ですね！😊" came
back on the first test. A speech synthesiser either reads its name out loud or
stumbles. The client strips them now, which is fine, but every client will
have to.

## 6. Two things that are not requests

**The Japanese comes apart.** "今日もお疲れさまで心からお手すように" is not a
sentence. That is Qwen3-0.6B at its size, not a server defect - worth knowing
when judging the loop, and an argument for a larger model if the NAS can hold
one.

**The LLM is the latency.** Measured on a real exchange:

| Stage | Time |
|---|---|
| ASR (SenseVoice) | 888 ms, RTF 0.40 |
| **LLM (Qwen3-0.6B)** | **8513 ms** |
| TTS (Piper Plus) | 2650 ms, RTF 0.34 |
| total | 12.3 s |

ASR and TTS are both comfortably faster than real time. Nothing in the audio
path is worth optimising until the eight and a half seconds in the middle is
addressed - by a smaller context, a quantisation that suits the CPU better, or
accepting it.
