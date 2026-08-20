"""Swappable speech and language backends for the companion server.

Each backend is deliberately small and behind a plain interface, because the
interesting part of this project is on the device, not here: whichever STT you
already run locally should be a twenty-line adapter, not a rewrite.

Everything speaks the same audio format as the firmware - 16 kHz, signed
16-bit, mono, little-endian - so no resampling happens anywhere in the path.
"""

from __future__ import annotations

import asyncio
import base64
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
        cutoff = 0.45 * dst_rate / src_rate
        taps = 63
        n = np.arange(taps) - (taps - 1) / 2
        h = np.sinc(2 * cutoff * n) * np.hamming(taps)
        h /= h.sum()
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
    async def stream(self, history: list[dict]) -> AsyncIterator[str]:
        last = history[-1]["content"] if history else ""
        yield f"[[happy]] I heard: {last}"


SYSTEM_PROMPT = """You are the voice of a small desktop companion robot built \
on an M5Stack M5GO. Answer the user's actual question or request directly in a \
natural spoken style. Do not merely repeat or paraphrase the user's input. Use \
as many sentences as the content genuinely needs; reply length is not limited \
by the M5 audio chunk size. Avoid markdown, emoji and code blocks unless the \
user explicitly needs them.

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


# The NAS synthesises the reply itself, so anything the model writes is spoken.
# The expression tag exists for the path where this server sees the text first
# and can strip it; send those instructions to the NAS and the companion reads
# "double square bracket happy" out loud.
VOICE_SYSTEM_PROMPT = """You are the voice of a small desktop companion robot. \
Answer the user's actual question or request directly, in a natural spoken \
style. Do not merely repeat or paraphrase what the user said. Use as many \
sentences as the content genuinely needs. Write only words that can be read \
aloud: no markdown, no emoji, no code, no bracketed stage directions."""


def voice_system_prompt(language: str | None = None) -> str:
    """The prompt for the endpoint that speaks without showing us the words."""
    if not language:
        return VOICE_SYSTEM_PROMPT
    name = LANGUAGE_NAMES.get(language, language)
    return f"{VOICE_SYSTEM_PROMPT}\n\nAlways reply in {name}, however the user writes."


def system_prompt(language: str | None = None) -> str:
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
    back over SPI. Moving the model one hop away, to a box on the same LAN, is
    as local as this architecture can get.
    """

    def __init__(self, model: str = "qwen3:0.6b",
                 host: str = "http://127.0.0.1:11434",
                 system: str = "", think: bool = False,
                 max_tokens: int | None = None, timeout: float = 120.0):
        import httpx

        self._client = httpx.AsyncClient(base_url=host, timeout=timeout)
        self._model = model
        self._system = system or SYSTEM_PROMPT
        self._think = think
        self._max_tokens = max_tokens if max_tokens and max_tokens > 0 else None

    async def stream(self, history: list[dict]) -> AsyncIterator[str]:
        options = {"temperature": 0.7}
        if self._max_tokens is not None:
            options["num_predict"] = self._max_tokens
        payload = {
            "model": self._model,
            "messages": [{"role": "system", "content": self._system}] + history,
            "stream": True,
            "think": self._think,
            "options": options,
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

    `max_tokens=None` omits the field rather than sending a number, so the
    backend's own completion length applies. That is not the same as sending a
    large value, and it is not the same as sending zero. A positive value is an
    explicit cap.
    """

    def __init__(self, model: str = "Qwen3-0.6B",
                 base_url: str = "http://127.0.0.1:11435/v1",
                 api_key: str = "", system: str = "",
                 max_tokens: int | None = None, timeout: float = 180.0):
        import httpx

        headers = {"Content-Type": "application/json"}
        if api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        self._client = httpx.AsyncClient(base_url=base_url.rstrip("/"),
                                         headers=headers, timeout=timeout)
        self._model = model
        self._system = system or SYSTEM_PROMPT
        self._max_tokens = max_tokens if max_tokens and max_tokens > 0 else None

    async def stream(self, history: list[dict]) -> AsyncIterator[str]:
        payload = {
            "model": self._model,
            "messages": [{"role": "system", "content": self._system}] + history,
            "stream": True,
            "temperature": 0.7,
        }
        if self._max_tokens is not None:
            payload["max_tokens"] = self._max_tokens
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
                chunk = choices[0].get("delta", {}).get("content")
                if chunk:
                    yield chunk


# ------------------------------------------------------- QnapAssistant ------
class QnapStt:
    def __init__(self, base_url: str, timeout: float = 120.0):
        import httpx

        self._client = httpx.AsyncClient(base_url=base_url.rstrip("/"), timeout=timeout)

    async def transcribe(self, pcm: bytes) -> str:
        response = await self._client.post(
            "/audio/transcriptions", content=pcm_to_wav(pcm),
            headers={"Content-Type": "audio/wav"})
        response.raise_for_status()
        body = response.json()
        return re.sub(r"<\|[^|]*\|>", "", body.get("text", "")).strip()


class QnapTts:
    def __init__(self, base_url: str, backend: str = "piper_plus",
                 language: str = "ja", speed: float = 1.0, timeout: float = 120.0,
                 peak: float = 0.0, profile: str = "m5go"):
        import httpx

        self._client = httpx.AsyncClient(base_url=base_url.rstrip("/"), timeout=timeout)
        self._backend = backend
        self._language = language
        self._speed = speed
        self._peak = peak
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
    if not pcm:
        return pcm
    import numpy as np

    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    if x.size < 64:
        return pcm

    taps = 127
    n = np.arange(taps) - (taps - 1) / 2
    lp = np.sinc(2 * (highpass_hz / SAMPLE_RATE) * n) * np.hamming(taps)
    lp /= lp.sum()
    hp = -lp
    hp[(taps - 1) // 2] += 1.0
    pad = taps // 2
    padded = np.concatenate([x[pad:0:-1], x, x[-2:-pad - 2:-1]])
    x = np.convolve(padded, hp, mode="valid")[:x.size].astype(np.float32)

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


class MultipartReader:
    """Incremental reader for `multipart/mixed`, fed arbitrary byte runs."""

    def __init__(self, boundary: bytes):
        self._delim = b"--" + boundary
        self._buf = bytearray()
        self._started = False

    def feed(self, data: bytes):
        self._buf.extend(data)
        while True:
            if not self._started:
                at = self._buf.find(self._delim)
                if at < 0:
                    break
                del self._buf[:at + len(self._delim)]
                self._started = True
            if len(self._buf) < 2:
                break
            if self._buf[:2] == b"--":
                return
            split = self._buf.find(b"\r\n\r\n")
            if split < 0:
                break
            head = bytes(self._buf[:split])
            headers = {}
            for line in head.decode("utf-8", "replace").splitlines():
                key, _, value = line.partition(":")
                if value:
                    headers[key.strip().lower()] = value.strip()
            length = headers.get("content-length")
            if length is None:
                break
            want = int(length)
            start = split + 4
            if len(self._buf) < start + want:
                break
            body = bytes(self._buf[start:start + want])
            del self._buf[:start + want]
            self._started = False
            yield headers, body


class QnapVoicePipeline:
    """QnapAssistant's ASR -> LLM -> TTS streaming endpoint.

    The completion may be long; QnapAssistant splits it into small speech
    chunks and emits each WAV part while the LLM continues generating.
    """

    def __init__(self, base_url: str, profile: str = "m5go", timeout: float = 180.0,
                 system: str = "", max_tokens: int | None = None):
        import httpx

        root = base_url.rstrip("/")
        if root.endswith("/v1"):
            root = root[: -len("/v1")]
        self._client = httpx.AsyncClient(base_url=root, timeout=timeout)
        self._profile = profile
        self._system = system
        self._max_tokens = max_tokens if max_tokens and max_tokens > 0 else None

    @staticmethod
    def _context_header(context: dict) -> str:
        raw = json.dumps(context, ensure_ascii=False,
                         separators=(",", ":")).encode("utf-8")
        return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")

    async def respond(self, pcm: bytes, *, history: list[dict] | None = None,
                      session_id: str = "", reset_session: bool = False) -> AsyncIterator[tuple]:
        """Yields (transcript|text|audio|done, value) as parts arrive."""
        params = {"profile": self._profile} if self._profile else None
        context = {}
        if self._system:
            context["system"] = self._system
        if self._max_tokens is not None:
            context["max_tokens"] = self._max_tokens
        if history:
            context["history"] = history
        if session_id:
            context["session_id"] = session_id
        if reset_session:
            context["reset_session"] = True

        headers = {"Content-Type": "audio/wav"}
        if context:
            headers["X-Qnap-Voice-Context"] = self._context_header(context)

        async with self._client.stream(
            "POST", "/v1/voice/chat/stream", params=params,
            content=pcm_to_wav(pcm), headers=headers,
        ) as response:
            if response.status_code >= 400:
                body = (await response.aread()).decode("utf-8", "replace")
                raise RuntimeError(f"{response.status_code} from the NAS: {body[:200]}")

            content_type = response.headers.get("content-type", "")
            boundary = ""
            for piece in content_type.split(";"):
                key, _, value = piece.strip().partition("=")
                if key.lower() == "boundary":
                    boundary = value.strip('"')
            if not boundary:
                raise RuntimeError(f"no multipart boundary in {content_type!r}")

            reader = MultipartReader(boundary.encode())
            async for data in response.aiter_bytes():
                for part_headers, body in reader.feed(data):
                    kind = part_headers.get("x-qnap-part-type", "")
                    if kind == "audio":
                        yield "audio", wav_to_pcm16(body)
                    elif kind in ("transcript", "text", "done", "meta"):
                        try:
                            payload = json.loads(body)
                        except json.JSONDecodeError:
                            continue
                        if kind == "meta":
                            continue
                        yield kind, payload
