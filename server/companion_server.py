#!/usr/bin/env python3
"""M5Companion server - speech in, thought, speech out.

The M5GO is the body: screen, microphone, speaker, buttons, IMU, LEDs. This is
the head. It owns the parts that need more than 520 KiB of RAM, and nothing
else - the device stays useful, animated and responsive even when this process
is not running.

    python server/companion_server.py --mock            # no models needed
    python server/companion_server.py --stt whisper --tts espeak

Protocol: see docs/PROTOCOL.md.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import re
import signal
import time
from pathlib import Path

import websockets

import backends as be

log = logging.getLogger("companion")

# 20 ms of 16 kHz mono PCM16, matching the firmware's chunk size exactly.
CHUNK_BYTES = 640
CHUNK_SECONDS = CHUNK_BYTES / (be.SAMPLE_RATE * be.SAMPLE_WIDTH)
# The device's jitter buffer is 24 chunks - 480 ms - and a chunk that arrives
# when it is full is dropped, not queued. Sending faster than real time
# therefore only works until the surplus fills it: at the 16 ms this used to
# use, 4 ms of surplus per chunk saturates the buffer after 2.4 seconds, and
# every chunk after that is lost. That is inaudible on a one-second beep and
# ruins a seven-second sentence, which is what a real voice actually sends.
# So: prime the buffer, then pace at exactly real time.
PRIME_CHUNKS = 8

EXPRESSION_TAG = re.compile(r"\[\[\s*([a-z_]+)\s*\]\]")
KNOWN_EXPRESSIONS = {
    "neutral", "happy", "excited", "thinking", "listening",
    "speaking", "confused", "sleepy", "playful", "error",
}
# A sentence is the unit of speech synthesis: short enough to start talking
# quickly, long enough that prosody does not fall apart.
SENTENCE_END = re.compile(r"(?<=[.!?。！？])\s+|\n+")
# Emoji, pictographs, dingbats and the variation selectors that follow them.
# Everything here is something a synthesiser cannot say out loud.
UNSPEAKABLE = re.compile(
    "[\U0001F000-\U0001FAFF\U00002190-\U000021FF\U00002300-\U000027BF"
    "\U0001F1E6-\U0001F1FF\U0000FE00-\U0000FE0F\U00002B00-\U00002BFF]+")


class Session:
    """One connected M5GO."""

    def __init__(self, ws, app: "CompanionApp"):
        self.ws = ws
        self.app = app
        self.name = "m5go"
        self.utterance = bytearray()
        self.listening = False
        self.capture_rate = be.SAMPLE_RATE
        # One conversation per device, kept on the NAS across reconnects: a
        # companion that forgets what was said because the Wi-Fi blinked is not
        # carrying a conversation. A fresh process starts the session over,
        # which is what --keep-session turns off.
        self.session_id = app.session_id
        self.reset_session = app.reset_session
        self.history: list[dict] = []
        self.speaking_task: asyncio.Task | None = None

    # ------------------------------------------------------------- sending --
    async def send_json(self, payload: dict):
        await self.ws.send(json.dumps(payload, ensure_ascii=False))

    async def set_expression(self, name: str, duration_ms: int = 2000):
        if name in KNOWN_EXPRESSIONS:
            await self.send_json({"type": "expression", "name": name,
                                  "duration_ms": duration_ms})

    async def speak(self, pcm: bytes):
        """Streams one utterance to the device, paced like real time."""
        if not pcm:
            return
        await self.send_json({"type": "speech.begin", "format": "pcm_s16le",
                              "rate": be.SAMPLE_RATE})
        try:
            started = time.monotonic()
            for index, offset in enumerate(range(0, len(pcm), CHUNK_BYTES)):
                await self.ws.send(bytes(pcm[offset:offset + CHUNK_BYTES]))
                if index < PRIME_CHUNKS:
                    continue
                # Absolute deadlines rather than a fixed sleep: asyncio rounds
                # a sleep up, and over a few hundred chunks that drift is
                # another way to underrun the buffer.
                ahead = started + (index - PRIME_CHUNKS + 1) * CHUNK_SECONDS - time.monotonic()
                if ahead > 0:
                    await asyncio.sleep(ahead)
        finally:
            await self.send_json({"type": "speech.end"})

    # ------------------------------------------------------------ receiving --
    def stream_speech(self) -> "SpeechStream":
        return SpeechStream(self, self.app.speech_lead)

    async def on_binary(self, data: bytes):
        if self.listening:
            self.utterance.extend(data)

    async def on_text(self, raw: str):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            log.warning("non-JSON text frame: %.60s", raw)
            return

        kind = msg.get("type", "")
        if kind == "hello":
            self.name = msg.get("name", "m5go")
            reset = msg.get("reset", "")
            log.info("hello from %s (fw %s, protocol %s%s)",
                     self.name, msg.get("fw"), msg.get("protocol"),
                     f", last reset: {reset}" if reset else "")
            if reset in ("panic", "task_wdt", "int_wdt", "other_wdt", "brownout"):
                # Not a fresh start someone asked for. Worth saying loudly,
                # because from across the room a crash and a power cycle look
                # exactly alike.
                log.warning("the device came back from %s", reset)
            await self.set_expression("happy", 1200)

        elif kind == "listen.begin":
            self.utterance.clear()
            # The microphone does not have to run at the rate the speaker does.
            # On the M5GO it cannot: reading the ADC through I2S gives a
            # fraction of the configured rate, so the firmware reads it
            # directly at 12 kHz and says so here.
            self.capture_rate = int(msg.get("rate") or be.SAMPLE_RATE)
            self.listening = True
            if self.speaking_task and not self.speaking_task.done():
                # The user pressed the button while we were talking. They win.
                self.speaking_task.cancel()
            await self.send_json({"type": "state", "state": "listening",
                                  "expression": "listening"})

        elif kind == "listen.end":
            self.listening = False
            if msg.get("cancelled"):
                self.utterance.clear()
                return
            audio = bytes(self.utterance)
            self.utterance.clear()
            self.speaking_task = asyncio.create_task(self.handle_utterance(audio))

        elif kind == "device.state":
            log.debug("device state=%s expression=%s",
                      msg.get("state"), msg.get("expression"))

        elif kind == "device.telemetry":
            log.info("telemetry: battery=%s%% heap=%s fps=%s rssi=%s",
                     msg.get("battery"), msg.get("heap"), msg.get("fps"), msg.get("rssi"))

    # ---------------------------------------------------------- the pipeline --
    async def handle_utterance(self, audio: bytes):
        captured = len(audio)
        seconds = captured / (self.capture_rate * be.SAMPLE_WIDTH)
        if self.capture_rate != be.SAMPLE_RATE:
            audio = be.resample_linear(audio, self.capture_rate, be.SAMPLE_RATE)
        # Both counts, and which rate each is at. Logging the duration from
        # before the conversion beside the byte count from after it reads as a
        # rate mismatch that is not there, and costs an hour to disprove.
        log.info("utterance: %.2f s (%d B @ %d Hz -> %d B @ %d Hz)",
                 seconds, captured, self.capture_rate, len(audio), be.SAMPLE_RATE)
        if seconds < 0.25:
            # Send the state back too. Without it the device sits in LISTENING
            # for ever, because nothing else ever tells it to stop - which
            # looks like a hang and is not one.
            await self.set_expression("confused", 1200)
            await self.send_json({"type": "state", "state": "idle"})
            return

        if self.app.dump_dir:
            self.app.dump_dir.mkdir(parents=True, exist_ok=True)
            path = self.app.dump_dir / f"utterance-{int(time.time())}.wav"
            path.write_bytes(be.pcm_to_wav(audio))
            log.info("saved %s", path)

        await self.send_json({"type": "state", "state": "thinking",
                              "expression": "thinking"})

        if self.app.voice is not None:
            await self.handle_utterance_streamed(audio)
            return

        try:
            text = await self.app.stt.transcribe(audio)
        except Exception:
            log.exception("transcription failed")
            await self.set_expression("error", 2500)
            await self.send_json({"type": "state", "state": "idle"})
            return

        log.info("heard: %s", text or "(nothing)")
        # Speech recognition returns punctuation for a room with nobody in it.
        # Sending that to a small model gets a confident answer to a question
        # nobody asked - the first time it happened the reply was a sentence
        # lifted straight out of the system prompt.
        if text and not re.search(r"[\w\u3040-\u30ff\u4e00-\u9fff]", text):
            log.info("nothing but punctuation; ignoring")
            text = ""
        if not text:
            await self.set_expression("confused", 1600)
            await self.send_json({"type": "state", "state": "idle"})
            return

        self.history.append({"role": "user", "content": text})
        # Keep the context bounded; this is a companion, not an archive.
        del self.history[:-self.app.history_turns * 2]

        reply = ""
        pending = ""
        expression_sent = False
        try:
            async for delta in self.app.llm.stream(self.history):
                reply += delta
                pending += delta

                if not expression_sent:
                    match = EXPRESSION_TAG.search(pending)
                    if match:
                        await self.set_expression(match.group(1), 3000)
                        expression_sent = True
                        pending = pending[match.end():]
                    elif len(pending) > 40:
                        expression_sent = True  # no tag coming; stop looking

                # Speak sentence by sentence so the first words leave quickly.
                while expression_sent:
                    split = SENTENCE_END.search(pending)
                    if not split:
                        break
                    sentence, pending = pending[:split.end()].strip(), pending[split.end():]
                    if sentence:
                        await self.say(sentence)

            tail = EXPRESSION_TAG.sub("", pending).strip()
            if tail:
                await self.say(tail)
        except asyncio.CancelledError:
            log.info("reply interrupted by the user")
            raise
        except Exception:
            log.exception("language model failed")
            await self.set_expression("error", 2500)
        else:
            cleaned = EXPRESSION_TAG.sub("", reply).strip()
            log.info("said: %s", cleaned)
            self.history.append({"role": "assistant", "content": cleaned})
        finally:
            await self.send_json({"type": "state", "state": "idle"})

    async def handle_utterance_streamed(self, audio: bytes):
        """The NAS does recognition, thought and synthesis in one request.

        The reply is synthesised on the NAS, so no expression tag is asked for
        on this path - the model would say it out loud. The face follows the
        state changes instead.

        The conversation is carried by session_id rather than by sending the
        history each turn. The NAS keeps the recent messages verbatim and
        summarises what falls off the end, which is the only version of this
        that survives a long conversation: the turns accumulate but the prompt
        does not.
        """
        stream = self.stream_speech()
        heard = ""
        reply = ""
        try:
            async for kind, payload in self.app.voice.respond(
                    audio, session_id=self.session_id,
                    reset_session=self.reset_session):
                self.reset_session = False
                if kind == "transcript":
                    heard = payload.get("transcript") or ""
                    log.info("heard: %s (%s ms)", heard or "(nothing)",
                             payload.get("timings", {}).get("asr_wall_ms"))
                elif kind == "text":
                    log.info("saying: %s", payload.get("text", ""))
                elif kind == "audio":
                    if not stream.spoke and not stream.buffer:
                        await self.set_expression("speaking", 3000)
                    stream.feed(payload)
                elif kind == "done":
                    reply = payload.get("reply", "")
                    timings = payload.get("timings", {})
                    log.info("said: %s (%d chars, first audio %s ms, total %s ms)",
                             reply, len(reply),
                             timings.get("first_audio_ready_ms"),
                             timings.get("total_wall_ms") or timings.get("wall_ms"))
        except asyncio.CancelledError:
            log.info("reply interrupted by the user")
            raise
        except Exception:
            log.exception("voice pipeline failed")
            await self.set_expression("error", 2500)
        finally:
            await stream.close()
            if not stream.spoke:
                await self.set_expression("confused", 1600)
            elif stream.stalls:
                log.info("the speaker ran dry %d time(s); the lead was %.1f s",
                         stream.stalls, self.app.speech_lead)
            # The NAS owns the conversation on this path; this copy is for the
            # log and for anything that switches back to the split pipeline
            # mid-session.
            if heard:
                self.history.append({"role": "user", "content": heard})
            if reply:
                self.history.append({"role": "assistant", "content": reply})
            del self.history[:-self.app.history_turns * 2]
            await self.send_json({"type": "state", "state": "idle"})

    async def say(self, sentence: str):
        # A small model often repeats the tag mid-reply. It is a stage
        # direction, not something to read out loud.
        sentence = EXPRESSION_TAG.sub("", sentence).strip()
        # Neither is an emoji. Qwen3 ends a friendly reply with one, and a
        # speech synthesiser either reads its name or stumbles over it.
        sentence = UNSPEAKABLE.sub("", sentence).strip()
        if not sentence:
            return
        try:
            pcm = await self.app.tts.synthesize(sentence)
        except Exception:
            log.exception("synthesis failed")
            return
        await self.speak(pcm)


class SpeechStream:
    """One continuous reply, assembled from sentences that arrive apart.

    The NAS synthesises each sentence as the model finishes it, so the audio
    comes in bursts separated by however long the next sentence takes to think
    of - seconds, on a model producing five tokens a second. Sending each burst
    as its own utterance meant the speaker stopped between them: the device
    drained its queue, re-armed its pre-roll and waited, and the reply came out
    in pieces with silence in the joins.

    So the reply is one utterance from the device's point of view - one
    speech.begin, one speech.end - and the gaps are absorbed here, where memory
    is not scarce. A lead is banked before any of it is sent, and the pacing
    then plays that lead out at the rate the speaker consumes it. A generation
    gap shorter than the lead is not heard at all; a longer one still shows,
    but shortened by the lead. The device's own queue holds 480 ms and cannot
    be the place this happens.
    """

    def __init__(self, session: "Session", lead_seconds: float):
        self.session = session
        self.lead_bytes = int(lead_seconds * be.SAMPLE_RATE * be.SAMPLE_WIDTH)
        self.buffer = bytearray()
        self.closed = False
        self.arrived = asyncio.Event()
        self.writer: asyncio.Task | None = None
        self.spoke = False
        self.stalls = 0

    def feed(self, pcm: bytes):
        if not pcm:
            return
        self.buffer.extend(pcm)
        self.arrived.set()
        if self.writer is None:
            self.writer = asyncio.create_task(self._run())

    async def close(self):
        self.closed = True
        self.arrived.set()
        if self.writer is not None:
            await self.writer

    async def _bank_the_lead(self):
        while not self.closed and len(self.buffer) < self.lead_bytes:
            self.arrived.clear()
            try:
                await asyncio.wait_for(self.arrived.wait(), timeout=0.2)
            except asyncio.TimeoutError:
                pass

    def _take(self) -> bytes | None:
        if len(self.buffer) >= CHUNK_BYTES:
            chunk = bytes(self.buffer[:CHUNK_BYTES])
            del self.buffer[:CHUNK_BYTES]
            return chunk
        if self.closed and self.buffer:
            chunk = bytes(self.buffer)
            self.buffer.clear()
            return chunk
        return None

    async def _run(self):
        await self._bank_the_lead()
        if not self.buffer:
            return
        self.spoke = True
        await self.session.send_json({"type": "speech.begin",
                                      "format": "pcm_s16le",
                                      "rate": be.SAMPLE_RATE})
        origin = time.monotonic()
        sent = 0
        try:
            while True:
                chunk = self._take()
                if chunk is None:
                    if self.closed:
                        break
                    # The lead ran out: the model is slower than the speaker.
                    # Wait for more and restart the clock, because pacing from
                    # the old origin would fire the backlog off in a burst and
                    # overrun the device's queue.
                    self.stalls += 1
                    self.arrived.clear()
                    await self.arrived.wait()
                    origin = time.monotonic()
                    sent = 0
                    continue
                await self.session.ws.send(chunk)
                sent += 1
                if sent <= PRIME_CHUNKS:
                    continue
                # Absolute deadlines rather than a fixed sleep: asyncio rounds a
                # sleep up, and over a few hundred chunks that drift is another
                # way to underrun the buffer.
                ahead = origin + (sent - PRIME_CHUNKS) * CHUNK_SECONDS - time.monotonic()
                if ahead > 0:
                    await asyncio.sleep(ahead)
        finally:
            await self.session.send_json({"type": "speech.end"})


class CompanionApp:
    def __init__(self, stt, llm, tts, dump_dir: Path | None, history_turns: int,
                 session_id: str = "", reset_session: bool = False,
                 speech_lead: float = 1.5, voice=None):
        self.stt = stt
        self.llm = llm
        self.tts = tts
        # When set, one streaming request replaces all three of the above.
        self.voice = voice
        self.dump_dir = dump_dir
        self.history_turns = history_turns
        self.session_id = session_id
        self.reset_session = reset_session
        self.speech_lead = speech_lead

    async def handle(self, ws):
        peer = getattr(ws, "remote_address", None)
        log.info("connected %s", peer)
        session = Session(ws, self)
        try:
            async for message in ws:
                if isinstance(message, bytes):
                    await session.on_binary(message)
                else:
                    await session.on_text(message)
        except websockets.ConnectionClosed:
            pass
        finally:
            if session.speaking_task:
                session.speaking_task.cancel()
            log.info("disconnected %s", peer)


def build_voice(args):
    if not args.qnap or not args.qnap_stream:
        return None
    return be.QnapVoicePipeline(args.openai_base_url, profile=args.qnap_profile,
                                system=be.voice_system_prompt(args.language),
                                max_tokens=args.qnap_max_tokens)


def build_backends(args):
    if args.mock:
        args.stt, args.llm = "none", "echo"

    if args.qnap:
        base = args.qnap.rstrip("/")
        if not base.endswith("/v1"):
            base += "/v1"
        args.openai_base_url = base
        if args.stt == "whisper":
            args.stt = "qnap"
        if args.tts == "espeak":
            args.tts = "qnap"
        if args.llm == "claude":
            args.llm = "openai"

    if args.stt == "whisper":
        stt = be.WhisperStt(args.whisper_model, args.language, args.whisper_compute)
    elif args.stt == "qnap":
        stt = be.QnapStt(args.openai_base_url)
    else:
        stt = be.NullStt()

    system = be.system_prompt(args.language)
    if args.llm == "claude":
        llm = be.ClaudeLlm(model=args.model, effort=args.effort, system=system)
    elif args.llm == "ollama":
        llm = be.OllamaLlm(model=args.ollama_model, host=args.ollama_host,
                           think=args.ollama_think, system=system)
    elif args.llm == "openai":
        llm = be.OpenAiLlm(model=args.openai_model, base_url=args.openai_base_url,
                           api_key=args.openai_key, system=system)
    else:
        llm = be.EchoLlm()

    if args.tts == "qnap":
        tts = be.QnapTts(args.openai_base_url, backend=args.qnap_voice,
                         language=args.language or "ja", peak=args.speaker_peak,
                         profile=args.qnap_profile)
    elif args.tts == "piper":
        tts = be.PiperTts(args.piper_voice)
    elif args.tts == "espeak":
        tts = be.EspeakTts(args.espeak_voice)
    elif args.tts == "tone":
        tts = be.ToneTts()
    else:
        tts = be.NullTts()

    return stt, llm, tts


async def main_async(args):
    stt, llm, tts = build_backends(args)
    app = CompanionApp(stt, llm, tts, args.dump_dir, args.history_turns,
                       session_id=args.session_id,
                       reset_session=not args.keep_session,
                       speech_lead=args.speech_lead,
                       voice=build_voice(args))

    stop = asyncio.get_running_loop().create_future()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, lambda: stop.done() or stop.set_result(None))
        except NotImplementedError:
            pass

    log.info("listening on ws://%s:%d%s", args.host, args.port, args.path)
    if app.voice is not None:
        log.info("voice pipeline=%s (speech in, thought and speech out in one "
                 "streaming request)", type(app.voice).__name__)
    else:
        log.info("stt=%s llm=%s tts=%s",
                 type(stt).__name__, type(llm).__name__, type(tts).__name__)

    # No permessage-deflate. The firmware's WebSocket client does not
    # implement it, and offering it produces a handshake the server considers
    # successful and the client quietly does not: the device then sits there
    # sending nothing, times out after its five second read deadline, and
    # reconnects forever. Compression buys nothing here anyway - the payload
    # is PCM.
    async with websockets.serve(app.handle, args.host, args.port,
                                max_size=2 ** 20, ping_interval=20,
                                compression=None):
        await stop
    log.info("shutting down")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--path", default="/m5companion", help="informational; any path is accepted")
    ap.add_argument("--mock", action="store_true",
                    help="no models: echoes what it heard, for testing the audio path")

    ap.add_argument("--qnap", default="", metavar="URL",
                    help="a QnapAssistant NAS, e.g. http://192.168.68.57:11435 - "
                         "sets speech in, the model and speech out to it at once")
    ap.add_argument("--qnap-voice", default="piper_plus", help="TTS backend on the NAS")
    ap.add_argument("--qnap-stream", action=argparse.BooleanOptionalAction, default=True,
                    help="use the NAS's streaming voice endpoint, which starts "
                         "speaking on the first sentence instead of the last")
    ap.add_argument("--speech-lead", type=float, default=1.5,
                    help="seconds of the reply to bank before the device starts "
                         "speaking it. The NAS produces a sentence at a time and "
                         "thinks between them, so without a lead the speaker "
                         "stops in the joins. A gap shorter than this is not "
                         "heard; the cost is that the reply starts this much "
                         "later. 0 sends each burst the moment it arrives")
    ap.add_argument("--qnap-max-tokens", type=int, default=0,
                    help="cap the NAS's reply at this many tokens. 0 sends no "
                         "limit at all, which is not the same as sending zero: "
                         "the NAS then applies whatever it is configured with, "
                         "and /api/voice/protocol reports what that is. It was "
                         "48 when this was written, which is why the replies "
                         "were arriving a sentence long")
    ap.add_argument("--session-id", default="kizuna",
                    help="names the conversation on the NAS. The recent turns "
                         "are kept verbatim there and the older ones are "
                         "summarised, so the conversation can outlast both a "
                         "reconnection and the context window")
    ap.add_argument("--keep-session", action="store_true",
                    help="continue the stored conversation instead of starting "
                         "it over when this process starts")
    ap.add_argument("--qnap-profile", default="m5go",
                    help="voice profile on the NAS. 'm5go' resamples to 16 kHz "
                         "with a proper filter and limits the peak; '' uses the "
                         "server's default")
    ap.add_argument("--speaker-peak", type=float, default=0.0, metavar="0..1",
                    help="override the profile's peak limit. The M5Stack Core has "
                         "8x of gain after the mixer, so anything near 1.0 clips")
    ap.add_argument("--stt", choices=["whisper", "qnap", "none"], default="whisper")
    ap.add_argument("--whisper-model", default="base")
    ap.add_argument("--whisper-compute", default="int8")
    ap.add_argument("--language", default=None, help="e.g. ja, en; auto-detect when unset")

    ap.add_argument("--llm", choices=["claude", "ollama", "openai", "echo"], default="claude")
    ap.add_argument("--model", default="claude-opus-5", help="Claude model id")
    ap.add_argument("--effort", choices=["low", "medium", "high"], default="low")
    ap.add_argument("--ollama-model", default="qwen3:0.6b")
    ap.add_argument("--ollama-host", default="http://127.0.0.1:11434")
    ap.add_argument("--ollama-think", action="store_true",
                    help="let a reasoning model think first; costs seconds of latency")
    ap.add_argument("--openai-model", default="Qwen3-0.6B",
                    help="model id for --llm openai; llama.cpp ignores it")
    ap.add_argument("--openai-base-url", default="http://127.0.0.1:11435/v1",
                    help="any OpenAI-compatible endpoint, e.g. a QnapAssistant NAS")
    ap.add_argument("--openai-key", default="", help="sent as a bearer token when set")

    ap.add_argument("--tts", choices=["piper", "espeak", "qnap", "tone", "none"], default="espeak",
                    help="tone synthesises beeps - useful for testing the audio "
                         "path on a machine with no TTS installed")
    ap.add_argument("--piper-voice", default="", help="path to a piper .onnx voice")
    ap.add_argument("--espeak-voice", default="en")

    ap.add_argument("--history-turns", type=int, default=8)
    ap.add_argument("--dump-dir", type=Path, default=None,
                    help="write each captured utterance here as a .wav")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
