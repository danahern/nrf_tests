"""Pipeline: BLE frames -> Opus decode -> PCM -> Whisper -> Ollama -> TEXT_CHUNK.

The same pipeline is exercised by both the real bleak transport and the
``--transport=file-inject`` simulator. Transport code pushes :class:`Frame`
instances into :meth:`AssistantPipeline.on_frame`; the pipeline owns decode,
transcription, LLM streaming, and outbound-frame emission.

Heavy native deps (opuslib, faster-whisper) are injected by the caller so the
test suite can run with lightweight fakes.
"""
from __future__ import annotations

import asyncio
import json
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import AsyncIterator, Awaitable, Callable, Protocol

import httpx

from .framing import Frame, FrameType, encode_frame


# ---------- config & results -------------------------------------------------


@dataclass
class PipelineConfig:
    sample_rate: int = 16000
    frame_ms: int = 20  # 20 ms Opus frames
    ollama_url: str = "http://localhost:11434"
    ollama_model: str = "glm-4.7"
    text_chunk_bytes: int = 32  # SDU-friendly 20-40 B per master plan
    dry_run_llm: bool = False

    @property
    def frame_samples(self) -> int:
        return self.sample_rate * self.frame_ms // 1000

    @property
    def frame_bytes(self) -> int:
        return self.frame_samples * 2  # int16 mono


@dataclass
class PipelineResult:
    audio_frames_encoded: int = 0
    transcript: str = ""
    reply_text: str = ""
    text_chunks_sent: int = 0


# ---------- injectable collaborators ----------------------------------------


class OpusCodec(Protocol):
    def encode(self, pcm_bytes: bytes, frame_size: int) -> bytes: ...
    def decode(self, opus_bytes: bytes, frame_size: int) -> bytes: ...


class Transcriber(Protocol):
    def transcribe(self, pcm: bytes, sample_rate: int) -> str: ...


class LLM(Protocol):
    def stream_reply(self, prompt: str) -> AsyncIterator[str]: ...


# Fakes/real implementations -------------------------------------------------


def build_fake_transcriber(text: str) -> Transcriber:
    class _Fake:
        def transcribe(self, pcm: bytes, sample_rate: int) -> str:
            return text

    return _Fake()


class DryRunLLM:
    """LLM stub that yields a canned reply without touching the network."""

    def __init__(self, reply: str = "This is a dry-run response."):
        self._reply = reply

    async def stream_reply(self, prompt: str) -> AsyncIterator[str]:
        # Split into a few tokens so downstream chunking is exercised.
        words = self._reply.split(" ")
        for i, w in enumerate(words):
            yield (w if i == 0 else " " + w)


class OllamaLLM:
    """Real /api/chat streaming client. Kept tiny; mocked in tests."""

    def __init__(self, base_url: str, model: str, timeout: float = 60.0):
        self._url = base_url.rstrip("/") + "/api/chat"
        self._model = model
        self._timeout = timeout

    async def stream_reply(self, prompt: str) -> AsyncIterator[str]:
        body = {
            "model": self._model,
            "messages": [{"role": "user", "content": prompt}],
            "stream": True,
        }
        async with httpx.AsyncClient(timeout=self._timeout) as client:
            async with client.stream("POST", self._url, json=body) as resp:
                resp.raise_for_status()
                async for line in resp.aiter_lines():
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    msg = obj.get("message") or {}
                    content = msg.get("content")
                    if content:
                        yield content
                    if obj.get("done"):
                        return


class FasterWhisperTranscriber:
    """Thin faster-whisper wrapper. Import lazily so tests don't need it."""

    def __init__(self, model_size: str = "base.en", device: str = "auto"):
        from faster_whisper import WhisperModel  # type: ignore

        self._model = WhisperModel(model_size, device=device)

    def transcribe(self, pcm: bytes, sample_rate: int) -> str:
        import numpy as np  # local — only needed on the real path

        samples = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
        segments, _info = self._model.transcribe(samples, language="en")
        return " ".join(s.text.strip() for s in segments).strip()


class OpuslibCodec:
    """Wraps opuslib for the real BLE path. Tests inject a fake."""

    def __init__(self, sample_rate: int = 16000, channels: int = 1, bitrate: int = 16000):
        import opuslib  # type: ignore

        self._enc = opuslib.Encoder(sample_rate, channels, "voip")
        self._enc.bitrate = bitrate
        self._dec = opuslib.Decoder(sample_rate, channels)

    def encode(self, pcm_bytes: bytes, frame_size: int) -> bytes:
        return self._enc.encode(pcm_bytes, frame_size)

    def decode(self, opus_bytes: bytes, frame_size: int) -> bytes:
        return self._dec.decode(opus_bytes, frame_size)


# ---------- chunking --------------------------------------------------------


def tokens_to_text_chunks(text: str, max_chunk: int) -> "list[bytes]":
    """Split UTF-8 ``text`` into chunks <= ``max_chunk`` bytes without
    splitting a multi-byte codepoint."""
    if not text:
        return []
    raw = text.encode("utf-8")
    out: list[bytes] = []
    i = 0
    while i < len(raw):
        end = min(i + max_chunk, len(raw))
        # Back off ``end`` if we'd split a UTF-8 continuation byte.
        while end < len(raw) and (raw[end] & 0xC0) == 0x80:
            end -= 1
        # Also back off if the byte _at_ end-1 starts a multi-byte sequence
        # whose continuations are past our window.
        if end < len(raw):
            b = raw[end - 1]
            if b & 0x80:
                # Walk back to the last lead byte and check its expected length.
                lead_idx = end - 1
                while lead_idx > i and (raw[lead_idx] & 0xC0) == 0x80:
                    lead_idx -= 1
                lead = raw[lead_idx]
                if lead & 0xE0 == 0xC0:
                    expected = 2
                elif lead & 0xF0 == 0xE0:
                    expected = 3
                elif lead & 0xF8 == 0xF0:
                    expected = 4
                else:
                    expected = 1
                if end - lead_idx < expected:
                    end = lead_idx
        if end == i:
            # Pathological: single codepoint wider than max_chunk — emit it
            # whole to make forward progress.
            end = i + 1
            while end < len(raw) and (raw[end] & 0xC0) == 0x80:
                end += 1
        out.append(raw[i:end])
        i = end
    return out


# ---------- the pipeline ----------------------------------------------------


OutboundCallback = Callable[[bytes], Awaitable[None]]


@dataclass
class _OutSeq:
    """Monotonic u8 seq counter for outbound frames (wraps at 256)."""

    value: int = 0

    def next(self) -> int:
        v = self.value
        self.value = (v + 1) & 0xFF
        return v


class AssistantPipeline:
    """Frame-level dispatcher + audio/LLM orchestration."""

    def __init__(
        self,
        config: PipelineConfig,
        opus: OpusCodec,
        transcriber: Transcriber,
        llm: LLM,
        on_outbound: OutboundCallback | None = None,
    ):
        self._cfg = config
        self._opus = opus
        self._transcriber = transcriber
        self._llm = llm
        self._on_outbound = on_outbound or self._default_outbound
        self._pcm_buf = bytearray()
        self._seq = _OutSeq()

    # ---------------- outbound helpers -----------------------------------

    async def _default_outbound(self, frame_bytes: bytes) -> None:
        # In file-inject mode the bridge replaces this with a stdout printer.
        pass

    async def _emit(self, ftype: FrameType, payload: bytes) -> None:
        raw = encode_frame(ftype, self._seq.next(), payload)
        await self._on_outbound(raw)

    # ---------------- inbound dispatch ----------------------------------

    async def on_frame(self, frame: Frame) -> PipelineResult | None:
        """Process a single inbound frame. Returns a :class:`PipelineResult`
        when STOP_LISTEN triggers a full transcribe→reply cycle."""
        if frame.type == FrameType.AUDIO:
            pcm = self._opus.decode(frame.payload, self._cfg.frame_samples)
            self._pcm_buf.extend(pcm)
            return None
        if frame.type == FrameType.CTRL_START_LISTEN:
            self._pcm_buf.clear()
            return None
        if frame.type == FrameType.CTRL_STOP_LISTEN:
            return await self._finish_utterance()
        # CTRL_STATE, TEXT_*, unknown-but-valid: ignored on the host ingress
        # path. They are emitted by the host, not consumed from the device.
        return None

    async def _finish_utterance(self, frames_counted: int | None = None) -> PipelineResult:
        pcm = bytes(self._pcm_buf)
        self._pcm_buf.clear()
        transcript = self._transcriber.transcribe(pcm, self._cfg.sample_rate)
        reply_text, chunks_sent = await self._stream_reply(transcript)
        return PipelineResult(
            audio_frames_encoded=frames_counted or 0,
            transcript=transcript,
            reply_text=reply_text,
            text_chunks_sent=chunks_sent,
        )

    async def _stream_reply(self, prompt: str) -> "tuple[str, int]":
        reply_parts: list[str] = []
        pending = ""
        chunks_sent = 0
        async for token in self._llm.stream_reply(prompt):
            reply_parts.append(token)
            pending += token
            # Flush whenever we have at least max_chunk bytes ready.
            while len(pending.encode("utf-8")) >= self._cfg.text_chunk_bytes:
                flush_chunks = tokens_to_text_chunks(pending, self._cfg.text_chunk_bytes)
                # Only flush all-but-the-last to allow the trailing piece to
                # grow with the next token.
                if len(flush_chunks) <= 1:
                    break
                for c in flush_chunks[:-1]:
                    await self._emit(FrameType.TEXT_CHUNK, c)
                    chunks_sent += 1
                pending = flush_chunks[-1].decode("utf-8")
        # Flush the tail.
        for c in tokens_to_text_chunks(pending, self._cfg.text_chunk_bytes):
            await self._emit(FrameType.TEXT_CHUNK, c)
            chunks_sent += 1
        await self._emit(FrameType.TEXT_END, b"")
        return "".join(reply_parts), chunks_sent

    # ---------------- file-inject simulator ------------------------------

    async def process_wav(self, wav_path: Path | str) -> PipelineResult:
        """Read a mono 16 kHz PCM WAV, chunk into 20 ms frames, encode Opus,
        wrap in AUDIO frames, feed the same ``on_frame`` path, then STOP."""
        path = Path(wav_path)
        with wave.open(str(path), "rb") as w:
            if w.getnchannels() != 1:
                raise ValueError("file-inject WAV must be mono")
            if w.getsampwidth() != 2:
                raise ValueError("file-inject WAV must be 16-bit PCM")
            if w.getframerate() != self._cfg.sample_rate:
                raise ValueError(
                    f"file-inject WAV sample rate {w.getframerate()} != "
                    f"expected {self._cfg.sample_rate}"
                )
            raw = w.readframes(w.getnframes())

        frame_bytes = self._cfg.frame_bytes
        frames_encoded = 0
        # Issue a synthetic START.
        await self.on_frame(Frame(FrameType.CTRL_START_LISTEN, 0, b""))
        for i in range(0, len(raw), frame_bytes):
            chunk = raw[i : i + frame_bytes]
            if len(chunk) < frame_bytes:
                break  # drop partial tail
            opus_bytes = self._opus.encode(chunk, self._cfg.frame_samples)
            await self.on_frame(
                Frame(FrameType.AUDIO, frames_encoded & 0xFF, opus_bytes)
            )
            frames_encoded += 1
        result = await self._finish_utterance(frames_counted=frames_encoded)
        return result
