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
import os
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
    """Linear resampling. Good enough for a 1 W speaker on a plastic box."""
    if src_rate == dst_rate or not pcm:
        return pcm
    import array

    src = array.array("h", pcm)
    n_out = int(len(src) * dst_rate / src_rate)
    out = array.array("h", bytes(n_out * 2))
    step = len(src) / n_out
    for i in range(n_out):
        pos = i * step
        j = int(pos)
        frac = pos - j
        a = src[j]
        b = src[j + 1] if j + 1 < len(src) else a
        out[i] = int(a + (b - a) * frac)
    return out.tobytes()


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

Pick the one that matches how the reply feels. Then write what you say. \
Example:

[[happy]] The build passed. All the tests are green.

If you did not understand the user, use [[confused]] and say so plainly \
rather than guessing."""


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
