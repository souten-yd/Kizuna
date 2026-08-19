# Direct QnapAssistant streaming on M5GO

The default transport remains the existing `companion_server` WebSocket bridge.
For a QNAP-hosted deployment, `voice_transport=qnap_stream` lets the M5GO talk
directly to QnapAssistant 0.4 without buffering either the recorded utterance or
the synthesized reply in RAM.

## Why

The classic ESP32 M5GO has no PSRAM and the measured live configuration has only
about 78 KB free heap. A multi-second WAV can be hundreds of kilobytes, and a
base64 JSON reply is larger still. Direct streaming keeps memory bounded:

- microphone PCM16 is uploaded as HTTP/1.1 chunked transfer in 20 ms blocks;
- QnapAssistant replies with the `m5go` multipart profile;
- JSON metadata parts are read into a small fixed buffer;
- each `audio/wav` part has only its 44-byte WAV header inspected;
- PCM is fed to the existing 24 x 20 ms playback queue as it arrives;
- reads are paced at the speaker rate, so TCP backpressure reaches the NAS
  instead of overflowing the ESP32 queue.

## device.json

```json
{
  "wifi_ssid": "YOUR_WIFI",
  "wifi_password": "YOUR_PASSWORD",
  "voice_transport": "qnap_stream",
  "qnap_host": "192.168.68.57",
  "qnap_port": 11435,
  "qnap_path": "/v1/voice/chat/stream?profile=m5go",
  "pack": "kizuna"
}
```

The setup portal exposes the same selector and QNAP fields. To go back to the
current bridge, choose `bridge`; the original `server_host`, `server_port` and
`server_path` settings remain untouched.

## Playback timing

This is chunk streaming, not token-to-waveform streaming. Qwen keeps generating
while the server waits for a natural 8-18-character M5GO text chunk. Piper then
synthesizes that chunk, and the first multipart WAV is sent immediately. The
M5GO starts playback after the existing ~80 ms preroll. While that first audio
plays, later LLM output and later Piper chunks continue to arrive.

So perceived latency becomes approximately:

```
end of speech
  + ASR
  + time to first text chunk
  + first Piper chunk
  + ~80 ms M5GO preroll
  = first audible reply
```

It no longer includes the time needed to generate and synthesize the entire
reply.

## Serial diagnostics

The direct client logs:

- `qnap uplink`: number of 20 ms PCM blocks sent;
- `qnap stream: first audio after ... ms`: client-side wait from request-body
  completion to first WAV part;
- `qnap done`: server-reported first-audio and total timings plus current heap.

The QnapAssistant `done` part also includes `llm_first_token_ms`,
`first_text_chunk_ms`, `first_audio_ready_ms`, `stream_chunks` and `total_ms`.
