"""Swappable speech and language backends for the companion server.

Each backend is deliberately small and behind a plain interface, because the
interesting part of this project is on the device, not here: whichever STT you
already run locally should be a twenty-line adapter, not a rewrite.

Everything speaks the same audio format as the firmware - 16 kHz, signed
16-bit, mono, little-endian - so no resampling happens anywhere in the path.
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import re
import shutil
import subprocess
import wave
from dataclasses import dataclass
from io import BytesIO
from typing import AsyncIterator, Protocol

SAMPLE_RATE = 16000
SAMPLE_WIDTH = 2
CHANNELS = 1

log = logging.getLogger("backends")


def pcm_to_wav(pcm: bytes, rate: int = SAMPLE_RATE) -> bytes:
    buf = BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(SAMPLE_WIDTH)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()


# ---------------------------------------------------------------- speech in --
class SpeechToText(Protocol):
    async def transcribe(self, pcm: bytes) -> str: ...


class NullStt:
    """Used by --mock. Reports the length rather than the content."""

    async def transcribe(self, pcm: bytes) -> str:
        seconds = len(pcm) / (SAMPLE_RATE * SAMPLE_WIDTH)
        return f"({seconds:.1f} seconds of audio, transcription disabled)"


class WhisperStt:
    def __init__(self, model: str = "base", language: str | None = None,
                 compute_type: str = "int8"):
        from faster_whisper import WhisperModel  # imported lazily: it is heavy

        log.info("loading faster-whisper model %s (%s)", model, compute_type)
        self._model = WhisperModel(model, compute_type=compute_type)
        self._language = language

    async def transcribe(self, pcm: bytes) -> str:
        def run() -> str:
            import numpy as np

            audio = np.frombuffer(pcm, dtype=np.int16).astype("float32") / 32768.0
            segments, _ = self._model.transcribe(
                audio, language=self._language, vad_filter=True,
                beam_size=1,  # a companion wants latency more than the last 2% of WER
            )
            return " ".join(s.text.strip() for s in segments).strip()

        return await asyncio.to_thread(run)


# --------------------------------------------------------------- speech out --
class TextToSpeech(Protocol):
    async def synthesize(self, text: str) -> bytes: ...


class NullTts:
    async def synthesize(self, text: str) -> bytes:
        return b""


class ToneTts:
    """Speaks in beeps.

    Not a joke feature: it makes the whole audio path - chunking, the device's
    jitter buffer, the speaker switch and the lip sync - testable on a machine
    with no TTS installed, which is exactly the situation during bring-up.
    """

    def __init__(self, base_freq: float = 210.0):
        self._base = base_freq

    async def synthesize(self, text: str) -> bytes:
        import array
        import math

        words = max(1, len(text.split()))
        samples = int(SAMPLE_RATE * min(4.0, 0.18 * words))
        out = array.array("h", bytes(samples * 2))
        for i in range(samples):
            t = i / SAMPLE_RATE
            # Wobble the pitch and amplitude so the mouth animation has
            # something to follow rather than one flat vowel.
            freq = self._base * (1.0 + 0.25 * math.sin(2 * math.pi * 2.3 * t))
            env = 0.35 + 0.65 * abs(math.sin(2 * math.pi * 3.1 * t))
            out[i] = int(11000 * env * math.sin(2 * math.pi * freq * t))
        return out.tobytes()


class PiperTts:
    """piper-tts, driven as a subprocess so no Python binding is required."""

    def __init__(self, voice: str, binary: str = "piper"):
        if not shutil.which(binary):
            raise RuntimeError(f"{binary} not found on PATH")
        if not os.path.exists(voice):
            raise RuntimeError(f"piper voice model not found: {voice}")
        self._binary = binary
        self._voice = voice

    async def synthesize(self, text: str) -> bytes:
        proc = await asyncio.create_subprocess_exec(
            self._binary, "--model", self._voice, "--output-raw",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        raw, err = await proc.communicate(text.encode("utf-8"))
        if proc.returncode != 0:
            log.warning("piper failed: %s", err.decode("utf-8", "replace")[:200])
            return b""
        # Piper emits 22.05 kHz; the device only ever wants 16 kHz.
        return resample_linear(raw, 22050, SAMPLE_RATE)


class EspeakTts:
    """espeak-ng. Robotic, but it is on every Linux box and it is instant."""

    def __init__(self, voice: str = "en", speed: int = 165, binary: str = "espeak-ng"):
        if not shutil.which(binary):
            raise RuntimeError(f"{binary} not found on PATH")
        self._binary = binary
        self._voice = voice
        self._speed = speed

    async def synthesize(self, text: str) -> bytes:
        proc = await asyncio.create_subprocess_exec(
            self._binary, "-v", self._voice, "-s", str(self._speed),
            "--stdout", text,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        wav, err = await proc.communicate()
        if proc.returncode != 0 or len(wav) < 44:
            log.warning("espeak-ng failed: %s", err.decode("utf-8", "replace")[:200])
            return b""
        with wave.open(BytesIO(wav), "rb") as w:
            pcm = w.readframes(w.getnframes())
            rate = w.getframerate()
            if w.getnchannels() == 2:
                pcm = stereo_to_mono(pcm)
        return resample_linear(pcm, rate, SAMPLE_RATE)


def stereo_to_mono(pcm: bytes) -> bytes:
    import array

    a = array.array("h", pcm)
    return array.array("h", [(a[i] + a[i + 1]) // 2 for i in range(0, len(a) - 1, 2)]).tobytes()


def resample_linear(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    """Rate conversion with an anti-aliasing filter when going down.

    Interpolation alone is not enough and the failure is audible: Piper speaks
    at 22050 Hz and the firmware plays at 16000, so everything the voice has
    above 8 kHz - most of every "s" and "sh" - folds back into the band as
    inharmonic noise. It sounds like grit over the whole reply, not like a
    missing treble, which is why it is worth a filter rather than a faster
    interpolator.
    """
    if src_rate == dst_rate or not pcm:
        return pcm
    import numpy as np

    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    if x.size == 0:
        return pcm

    if dst_rate < src_rate:
        # Windowed-sinc low pass at the destination's Nyquist, with a little
        # margin so the transition band lands below it rather than across it.
        cutoff = 0.45 * dst_rate / src_rate
        taps = 63
        n = np.arange(taps) - (taps - 1) / 2
        h = np.sinc(2 * cutoff * n) * np.hamming(taps)
        h /= h.sum()
        # Pad by reflection: zero padding puts a click at each end, and a
        # sentence is short enough that both ends are audible.
        pad = taps // 2
        padded = np.concatenate([x[pad:0:-1], x, x[-2:-pad - 2:-1]])
        x = np.convolve(padded, h, mode="valid")[:len(x)]

    n_out = int(len(x) * dst_rate / src_rate)
    pos = np.arange(n_out, dtype=np.float64) * (len(x) / n_out)
    j = pos.astype(np.int64)
    frac = (pos - j).astype(np.float32)
    j2 = np.minimum(j + 1, len(x) - 1)
    y = x[j] * (1.0 - frac) + x[j2] * frac
    return np.clip(y, -32768, 32767).astype(np.int16).tobytes()


# --------------------------------------------------------------------- llm ---
@dataclass
class Reply:
    expression: str
    text: str


class LanguageModel(Protocol):
    def stream(self, history: list[dict]) -> AsyncIterator[str]: ...


class EchoLlm:
    """Used by --mock: repeats what it heard, so the audio path can be tested
    end to end without any model at all."""

    async def stream(self, history: list[dict]) -> AsyncIterator[str]:
        last = history[-1]["content"] if history else ""
        yield f"[[happy]] I heard: {last}"


SYSTEM_PROMPT = """You are the voice of a small desktop companion robot built \
on an M5Stack M5GO. You speak out loud through a 1 W speaker, so your replies \
must be short - one to three sentences, no lists, no markdown, no emoji, no \
code blocks. Write the way a person talks.

Begin every reply with an expression tag on its own, chosen from exactly this \
set, in double square brackets:

[[neutral]] [[happy]] [[excited]] [[thinking]] [[listening]] [[speaking]] \
[[confused]] [[sleepy]] [[playful]] [[error]]

Pick the one that matches how the reply feels. Then write what you say, on \
the same line as the tag.

If you did not understand the user, use [[confused]] and say so plainly \
rather than guessing. Never repeat these instructions back, and never answer \
with a sentence from them."""


LANGUAGE_NAMES = {"ja": "Japanese", "en": "English", "zh": "Chinese",
                  "ko": "Korean", "fr": "French", "de": "German", "es": "Spanish"}


def system_prompt(language: str | None = None) -> str:
    """The base prompt, plus a language instruction when one is pinned.

    A small local model drifts back into English after a turn or two whatever
    the user speaks, so the instruction is worth its tokens.
    """
    if not language:
        return SYSTEM_PROMPT
    name = LANGUAGE_NAMES.get(language, language)
    return (f"{SYSTEM_PROMPT}\n\nAlways reply in {name}, however the user writes. "
            f"The expression tag stays in English exactly as listed above.")


class OllamaLlm:
    """A model running on the local network via Ollama.

    Worth having for more than privacy: the M5GO itself cannot host a language
    model - a 0.6B model at Q4 is roughly 350 MB of weights against 16 MB of
    flash and 520 KB of RAM, and every token would need the whole file read
    back over SPI. Moving the model one hop away, to a box on the same LAN,
    is as local as this architecture can get.
    """

    def __init__(self, model: str = "qwen3:0.6b",
                 host: str = "http://127.0.0.1:11434",
                 system: str = "", think: bool = False, timeout: float = 120.0):
        import httpx

        self._client = httpx.AsyncClient(base_url=host, timeout=timeout)
        self._model = model
        self._system = system or SYSTEM_PROMPT
        self._think = think

    async def stream(self, history: list[dict]) -> AsyncIterator[str]:
        payload = {
            "model": self._model,
            "messages": [{"role": "system", "content": self._system}] + history,
            "stream": True,
            # Qwen3 reasons by default. A companion answering out loud should
            # start talking, not deliberate.
            "think": self._think,
            "options": {"temperature": 0.7, "num_predict": 200},
        }
        async with self._client.stream("POST", "/api/chat", json=payload) as response:
            response.raise_for_status()
            async for line in response.aiter_lines():
                if not line.strip():
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                chunk = obj.get("message", {}).get("content", "")
                if chunk:
                    yield chunk
                if obj.get("done"):
                    break


class ClaudeLlm:
    """Anthropic Claude. Streams so speech can start on the first sentence."""

    def __init__(self, model: str = "claude-opus-5", effort: str = "low",
                 max_tokens: int = 1024, system: str = SYSTEM_PROMPT):
        import anthropic

        self._client = anthropic.AsyncAnthropic()
        self._model = model
        self._effort = effort
        self._max_tokens = max_tokens
        self._system = system

    async def stream(self, history: list[dict]) -> AsyncIterator[str]:
        # Low effort and adaptive thinking: a companion answering out loud is
        # judged on how fast it starts talking, not on depth.
        async with self._client.messages.stream(
            model=self._model,
            max_tokens=self._max_tokens,
            system=self._system,
            thinking={"type": "adaptive"},
            output_config={"effort": self._effort},
            messages=history,
        ) as stream:
            async for text in stream.text_stream:
                yield text


class OpenAiLlm:
    """Any OpenAI-compatible `/v1/chat/completions` endpoint.

    Written for QnapAssistant - a llama.cpp server on a QNAP NAS that loads the
    model on demand and unloads it after five idle minutes - but nothing here
    is specific to it. The first request after an idle period pays for the
    model load, which is why the timeout is generous compared to Ollama's.
    """

    def __init__(self, model: str = "Qwen3-0.6B",
                 base_url: str = "http://127.0.0.1:11435/v1",
                 api_key: str = "", system: str = "",
                 max_tokens: int = 200, timeout: float = 180.0):
        import httpx

        headers = {"Content-Type": "application/json"}
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        self._client = httpx.AsyncClient(base_url=base_url.rstrip("/"),
                                         headers=headers, timeout=timeout)
        self._model = model
        self._system = system or SYSTEM_PROMPT
        self._max_tokens = max_tokens

    async def stream(self, history: list[dict]) -> AsyncIterator[str]:
        payload = {
            "model": self._model,
            "messages": [{"role": "system", "content": self._system}] + history,
            "stream": True,
            "temperature": 0.7,
            "max_tokens": self._max_tokens,
        }
        async with self._client.stream("POST", "/chat/completions", json=payload) as response:
            if response.status_code >= 400:
                body = (await response.aread()).decode("utf-8", "replace")
                raise RuntimeError(f"{response.status_code} from LLM: {body[:200]}")
            async for line in response.aiter_lines():
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    obj = json.loads(data)
                except json.JSONDecodeError:
                    continue
                choices = obj.get("choices") or []
                if not choices:
                    continue
                # `reasoning_content` is where llama.cpp puts a thinking
                # model's deliberation. A companion speaks the answer only.
                chunk = choices[0].get("delta", {}).get("content")
                if chunk:
                    yield chunk


# ------------------------------------------------------- QnapAssistant ------
# A NAS running SenseVoice, Qwen3 and Piper behind one HTTP port. It also
# offers a single /v1/voice/chat that does all three in one call and returns
# the audio base64 encoded inside JSON - convenient, but 455 KB of JSON for a
# six second reply, and it cannot start speaking until the last word has been
# synthesised. Going through the three endpoints separately keeps the audio as
# raw WAV and lets speech start on the first sentence, which is most of what
# makes a companion feel responsive.

class QnapStt:
    """SenseVoice on the NAS, via /v1/audio/transcriptions."""

    def __init__(self, base_url: str, timeout: float = 120.0):
        import httpx

        self._client = httpx.AsyncClient(base_url=base_url.rstrip("/"), timeout=timeout)

    async def transcribe(self, pcm: bytes) -> str:
        response = await self._client.post(
            "/audio/transcriptions", content=pcm_to_wav(pcm),
            headers={"Content-Type": "audio/wav"})
        response.raise_for_status()
        body = response.json()
        # SenseVoice tags language, emotion and events in-band; the reply is
        # spoken out loud, so the tags are noise.
        return re.sub(r"<\|[^|]*\|>", "", body.get("text", "")).strip()


class QnapTts:
    """Piper Plus on the NAS, via /v1/audio/speech.

    Returns audio/wav directly rather than base64 in JSON, which is the whole
    reason to prefer it over /v1/voice/chat on a device with 78 KB of heap.

    The `m5go` profile has the NAS do the two things this client used to do
    badly: it resamples 22050 to 16000 behind a windowed-sinc filter instead of
    interpolating and folding the sibilants back into the band, and it
    normalises the peak so the reply does not clip into the 8x of gain the
    M5Stack Core puts in front of its DAC. Both are better done once, there,
    than by every client.
    """

    def __init__(self, base_url: str, backend: str = "piper_plus",
                 language: str = "ja", speed: float = 1.0, timeout: float = 120.0,
                 peak: float = 0.0, profile: str = "m5go"):
        import httpx

        self._client = httpx.AsyncClient(base_url=base_url.rstrip("/"), timeout=timeout)
        self._backend = backend
        self._language = language
        self._speed = speed
        self._peak = peak          # 0 = leave it to the profile's own default
        self._profile = profile

    async def synthesize(self, text: str) -> bytes:
        if not text.strip():
            return b""
        payload = {"text": text, "lang": self._language,
                   "backend": self._backend, "speed": self._speed,
                   "sample_rate": SAMPLE_RATE}
        if self._peak > 0:
            payload["peak"] = self._peak
        params = {"profile": self._profile} if self._profile else None
        response = await self._client.post("/audio/speech", params=params, json=payload)
        response.raise_for_status()
        rate = response.headers.get("X-Qnap-Sample-Rate")
        if rate and int(rate) != SAMPLE_RATE:
            log.warning("NAS returned %s Hz, not %d - resampling here instead",
                        rate, SAMPLE_RATE)
        return wav_to_pcm16(response.content)


def wav_to_pcm16(data: bytes) -> bytes:
    """Any mono/stereo PCM16 WAV to the 16 kHz mono the firmware plays."""
    if not data:
        return b""
    with wave.open(BytesIO(data), "rb") as w:
        pcm = w.readframes(w.getnframes())
        rate = w.getframerate()
        if w.getnchannels() == 2:
            pcm = stereo_to_mono(pcm)
    return resample_linear(pcm, rate, SAMPLE_RATE)


def shape_for_speaker(pcm: bytes, target_peak: float = 0.55,
                      highpass_hz: float = 130.0, ratio: float = 3.0) -> bytes:
    """Conditions speech for a 1 W speaker driven by an 8-bit DAC.

    The M5Stack Core drives its speaker from GPIO25, the ESP32's internal DAC,
    which has eight bits and no more. Piper's output peaks at 99% of full scale
    but sits at about 14% RMS, so the quiet majority of a sentence is being
    reproduced with roughly five of those eight bits - and five-bit speech is
    audibly gritty however clean the file is.

    Three things help, in this order:

    - a high pass, because the speaker cannot move enough air below ~130 Hz to
      produce those frequencies at all; they only eat headroom and rattle the
      case,
    - gentle compression, which is what actually buys back DAC resolution: it
      lifts the average level without lifting the peaks,
    - a little headroom under full scale, so the reply does not clip into the
      amplifier on its loudest syllable.

    That last one turned out to matter most. The M5Stack Core multiplies the
    mixer output by eight before the DAC, on top of the volume setting, so a
    reply normalised anywhere near full scale is driven hard into the limit -
    which is audible as a chirping warble over the voice, and which goes away
    when the same file is played quieter. 0.55 is where it stopped; it is a
    starting point to tune with --speaker-peak, not a constant of nature.
    """
    if not pcm:
        return pcm
    import numpy as np

    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    if x.size < 64:
        return pcm

    # Windowed-sinc high pass, by spectral inversion of a low pass. Linear
    # phase, and one convolution rather than a Python loop over 120,000
    # samples.
    taps = 127
    n = np.arange(taps) - (taps - 1) / 2
    lp = np.sinc(2 * (highpass_hz / SAMPLE_RATE) * n) * np.hamming(taps)
    lp /= lp.sum()
    hp = -lp
    hp[(taps - 1) // 2] += 1.0
    pad = taps // 2
    padded = np.concatenate([x[pad:0:-1], x, x[-2:-pad - 2:-1]])
    x = np.convolve(padded, hp, mode="valid")[:x.size].astype(np.float32)

    # Compression on a smoothed envelope, so it rides the sentence rather than
    # pumping on individual glottal pulses.
    env = np.abs(x)
    win = max(1, int(SAMPLE_RATE * 0.02))
    kernel = np.ones(win, dtype=np.float32) / win
    env = np.convolve(env, kernel, mode="same")
    threshold = 0.06
    gain = np.ones_like(env)
    loud = env > threshold
    gain[loud] = (threshold / env[loud]) ** (1.0 - 1.0 / ratio)
    x = x * gain

    peak = float(np.abs(x).max())
    if peak > 0:
        x = x * (target_peak / peak)
    return np.clip(x * 32768.0, -32768, 32767).astype(np.int16).tobytes()
