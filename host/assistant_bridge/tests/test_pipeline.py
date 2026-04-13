"""Tests for the audio -> transcript -> LLM -> TEXT_CHUNK pipeline.

These tests mock opuslib, faster-whisper, and the Ollama HTTP client so the
suite runs with zero network / hardware / native-lib dependencies.
"""
from __future__ import annotations

import asyncio
import struct
import wave
from pathlib import Path

import pytest

from assistant_bridge.framing import Frame, FrameType, decode_frame, encode_frame
from assistant_bridge.pipeline import (
    AssistantPipeline,
    DryRunLLM,
    PipelineConfig,
    build_fake_transcriber,
    tokens_to_text_chunks,
)


# ---------- helpers ----------------------------------------------------------


def _write_silence_wav(path: Path, seconds: float = 0.5, sample_rate: int = 16000):
    """Write a mono 16-bit PCM WAV of silence. Tiny, deterministic."""
    n = int(seconds * sample_rate)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(b"\x00\x00" * n)


# ---------- tokens_to_text_chunks -------------------------------------------


class TestTokensToTextChunks:
    def test_splits_long_text_into_bounded_chunks(self):
        text = "A" * 100
        chunks = list(tokens_to_text_chunks(text, max_chunk=32))
        assert all(len(c) <= 32 for c in chunks)
        assert b"".join(chunks).decode() == text

    def test_preserves_utf8_multibyte_boundaries(self):
        text = "héllo " * 10  # 'é' is 2 bytes in utf-8
        chunks = list(tokens_to_text_chunks(text, max_chunk=8))
        # Re-joining must decode cleanly — i.e. no codepoint split mid-byte.
        assert b"".join(chunks).decode("utf-8") == text
        assert all(len(c) <= 8 for c in chunks)

    def test_empty_text_yields_no_chunks(self):
        assert list(tokens_to_text_chunks("", max_chunk=32)) == []


# ---------- DryRunLLM --------------------------------------------------------


class TestDryRunLLM:
    @pytest.mark.asyncio
    async def test_yields_canned_reply_tokens(self):
        llm = DryRunLLM(reply="hello world")
        tokens = [t async for t in llm.stream_reply("ignored prompt")]
        assert "".join(tokens) == "hello world"
        # Canned reply must emit at least one token.
        assert len(tokens) >= 1


# ---------- AssistantPipeline.process_wav -----------------------------------


class _FakeOpus:
    """Fake opus encoder+decoder pair. Identity over raw PCM bytes."""

    def __init__(self):
        self.encoded = []
        self.decoded_samples = 0

    def encode(self, pcm_bytes: bytes, frame_size: int) -> bytes:
        # Pretend to encode — return a short stand-in so we can inspect
        # AUDIO frame payloads.
        tag = b"OP" + struct.pack("<H", len(pcm_bytes) // 2)
        self.encoded.append(pcm_bytes)
        return tag

    def decode(self, opus_bytes: bytes, frame_size: int) -> bytes:
        # Return silence of the requested frame_size samples.
        self.decoded_samples += frame_size
        return b"\x00\x00" * frame_size


@pytest.mark.asyncio
async def test_file_inject_produces_expected_frame_sequence(tmp_path):
    wav_path = tmp_path / "hello.wav"
    _write_silence_wav(wav_path, seconds=0.5)  # 25 frames of 20 ms

    fake_opus = _FakeOpus()
    outbound: list[bytes] = []

    async def on_outbound(frame_bytes: bytes):
        outbound.append(frame_bytes)

    cfg = PipelineConfig(
        sample_rate=16000,
        frame_ms=20,
        ollama_url="http://localhost:11434",
        ollama_model="glm-4.7",
        dry_run_llm=True,
    )
    pipeline = AssistantPipeline(
        config=cfg,
        opus=fake_opus,
        transcriber=build_fake_transcriber("what is the capital of france"),
        llm=DryRunLLM(reply="Paris is the capital."),
        on_outbound=on_outbound,
    )

    result = await pipeline.process_wav(wav_path)

    # Audio frames in + state/text frames out.
    assert result.audio_frames_encoded == 25
    assert result.transcript == "what is the capital of france"
    assert fake_opus.decoded_samples == 25 * 320  # 20 ms @ 16 kHz

    # Parse every outbound frame and inspect the sequence of types.
    parsed = [decode_frame(f)[0] for f in outbound]
    types = [f.type for f in parsed]

    # Must start with a CTRL_STATE transition into THINK (or similar),
    # then one or more TEXT_CHUNK frames, ending with TEXT_END.
    assert FrameType.TEXT_END in types
    assert types[-1] == FrameType.TEXT_END
    assert any(t == FrameType.TEXT_CHUNK for t in types)
    # Reassemble chunks → full reply.
    chunks = b"".join(f.payload for f in parsed if f.type == FrameType.TEXT_CHUNK)
    assert chunks.decode("utf-8") == "Paris is the capital."

    # Every frame must be a valid encode_frame round-trip.
    for raw in outbound:
        f, consumed = decode_frame(raw)
        assert consumed == len(raw)


@pytest.mark.asyncio
async def test_dispatcher_routes_incoming_audio_and_stop(tmp_path):
    """Feeding AUDIO + STOP_LISTEN frames to the dispatcher must drive the
    same pipeline path as file-inject."""
    fake_opus = _FakeOpus()
    outbound: list[Frame] = []

    async def on_outbound(frame_bytes: bytes):
        outbound.append(decode_frame(frame_bytes)[0])

    cfg = PipelineConfig(dry_run_llm=True)
    pipeline = AssistantPipeline(
        config=cfg,
        opus=fake_opus,
        transcriber=build_fake_transcriber("ping"),
        llm=DryRunLLM(reply="pong"),
        on_outbound=on_outbound,
    )

    # Push 3 AUDIO frames (opus payload is arbitrary here — fake_opus.decode
    # ignores contents).
    await pipeline.on_frame(Frame(FrameType.AUDIO, 1, b"xx"))
    await pipeline.on_frame(Frame(FrameType.AUDIO, 2, b"yy"))
    await pipeline.on_frame(Frame(FrameType.AUDIO, 3, b"zz"))
    await pipeline.on_frame(Frame(FrameType.CTRL_STOP_LISTEN, 4, b""))

    # Must have decoded 3 frames and emitted TEXT_CHUNK(s) + TEXT_END.
    assert fake_opus.decoded_samples == 3 * 320
    out_types = [f.type for f in outbound]
    assert out_types[-1] == FrameType.TEXT_END
    assert any(t == FrameType.TEXT_CHUNK for t in out_types)
    assert (
        b"".join(f.payload for f in outbound if f.type == FrameType.TEXT_CHUNK)
        == b"pong"
    )


@pytest.mark.asyncio
async def test_mocked_ollama_http_streaming(monkeypatch):
    """End-to-end pipeline but with a mocked httpx.AsyncClient standing in
    for the real Ollama server."""
    from assistant_bridge import pipeline as pipeline_mod

    class _FakeStreamResponse:
        def __init__(self, lines):
            self._lines = lines

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def raise_for_status(self):
            pass

        async def aiter_lines(self):
            for line in self._lines:
                yield line

    class _FakeAsyncClient:
        def __init__(self, *a, **kw):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def stream(self, method, url, json):
            # Emit Ollama-style NDJSON lines with {message:{content:...}}.
            lines = [
                '{"message":{"content":"Hel"}}',
                '{"message":{"content":"lo"}}',
                '{"message":{"content":" world"},"done":true}',
            ]
            return _FakeStreamResponse(lines)

    monkeypatch.setattr(pipeline_mod.httpx, "AsyncClient", _FakeAsyncClient)

    llm = pipeline_mod.OllamaLLM(base_url="http://x", model="glm-4.7")
    tokens = [t async for t in llm.stream_reply("hi")]
    assert "".join(tokens) == "Hello world"


@pytest.mark.asyncio
async def test_ollama_stream_skips_blank_and_malformed_lines(monkeypatch):
    from assistant_bridge import pipeline as pipeline_mod

    class _FakeStreamResponse:
        def __init__(self, lines):
            self._lines = lines

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def raise_for_status(self):
            pass

        async def aiter_lines(self):
            for line in self._lines:
                yield line

    class _FakeAsyncClient:
        def __init__(self, *a, **kw):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def stream(self, method, url, json):
            return _FakeStreamResponse(
                [
                    "",  # blank — skipped
                    "not json",  # malformed — skipped
                    '{"message":{}}',  # no content — skipped
                    '{"message":{"content":"ok"}}',
                    '{"done":true}',  # done flag ends stream
                    '{"message":{"content":"after-done"}}',  # must not appear
                ]
            )

    monkeypatch.setattr(pipeline_mod.httpx, "AsyncClient", _FakeAsyncClient)
    llm = pipeline_mod.OllamaLLM(base_url="http://x/", model="glm-4.7")
    tokens = [t async for t in llm.stream_reply("hi")]
    assert "".join(tokens) == "ok"


@pytest.mark.asyncio
async def test_stream_reply_flushes_midstream_when_pending_exceeds_chunk():
    """When a token pushes pending past max_chunk, mid-stream flush fires."""

    class _LongReplyLLM:
        async def stream_reply(self, prompt):
            # Emit chunks that cumulatively exceed text_chunk_bytes quickly.
            for _ in range(8):
                yield "abcdefghij"  # 10 bytes each, 80 total

    outbound = []

    async def on_outbound(raw):
        outbound.append(decode_frame(raw)[0])

    cfg = PipelineConfig(dry_run_llm=True, text_chunk_bytes=16)
    pipeline = AssistantPipeline(
        config=cfg,
        opus=_FakeOpus(),
        transcriber=build_fake_transcriber("x"),
        llm=_LongReplyLLM(),
        on_outbound=on_outbound,
    )
    await pipeline._finish_utterance()
    chunks = [f for f in outbound if f.type == FrameType.TEXT_CHUNK]
    # More than one chunk must have been sent, including at least one
    # mid-stream flush (not just the tail).
    assert len(chunks) >= 2
    assert all(len(c.payload) <= 16 for c in chunks)
    assert outbound[-1].type == FrameType.TEXT_END
    assert (
        b"".join(c.payload for c in chunks).decode() == "abcdefghij" * 8
    )


@pytest.mark.asyncio
async def test_ctrl_state_and_text_frames_are_ignored_on_ingress():
    cfg = PipelineConfig(dry_run_llm=True)
    pipeline = AssistantPipeline(
        config=cfg,
        opus=_FakeOpus(),
        transcriber=build_fake_transcriber("x"),
        llm=DryRunLLM(reply="y"),
    )
    # Host-side: CTRL_STATE / TEXT_* are outbound-only; they must no-op on ingress.
    assert await pipeline.on_frame(Frame(FrameType.CTRL_STATE, 0, b"\x00")) is None
    assert await pipeline.on_frame(Frame(FrameType.TEXT_CHUNK, 0, b"x")) is None
    assert await pipeline.on_frame(Frame(FrameType.TEXT_END, 0, b"")) is None


def test_out_seq_wraps_at_u8():
    from assistant_bridge.pipeline import _OutSeq

    s = _OutSeq(value=254)
    assert s.next() == 254
    assert s.next() == 255
    assert s.next() == 0  # wrapped


@pytest.mark.asyncio
async def test_process_wav_rejects_stereo(tmp_path):
    path = tmp_path / "stereo.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b"\x00" * 800)
    cfg = PipelineConfig(dry_run_llm=True)
    pipeline = AssistantPipeline(
        config=cfg, opus=_FakeOpus(), transcriber=build_fake_transcriber("x"), llm=DryRunLLM("y")
    )
    with pytest.raises(ValueError, match="mono"):
        await pipeline.process_wav(path)


@pytest.mark.asyncio
async def test_process_wav_rejects_non_16bit(tmp_path):
    path = tmp_path / "8bit.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(16000)
        w.writeframes(b"\x00" * 400)
    cfg = PipelineConfig(dry_run_llm=True)
    pipeline = AssistantPipeline(
        config=cfg, opus=_FakeOpus(), transcriber=build_fake_transcriber("x"), llm=DryRunLLM("y")
    )
    with pytest.raises(ValueError, match="16-bit"):
        await pipeline.process_wav(path)


def test_tokens_to_text_chunks_handles_codepoint_wider_than_max():
    # A 3-byte UTF-8 character (e.g. 中) with max_chunk=1 must still emit it whole.
    chunks = tokens_to_text_chunks("中", max_chunk=1)
    assert b"".join(chunks).decode("utf-8") == "中"


@pytest.mark.asyncio
async def test_process_wav_rejects_wrong_sample_rate(tmp_path):
    path = tmp_path / "bad.wav"
    # Write an 8 kHz WAV so the pipeline's 16 kHz check fires.
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(b"\x00\x00" * 800)

    cfg = PipelineConfig(dry_run_llm=True)
    pipeline = AssistantPipeline(
        config=cfg,
        opus=_FakeOpus(),
        transcriber=build_fake_transcriber("x"),
        llm=DryRunLLM(reply="y"),
    )
    with pytest.raises(ValueError, match="sample rate"):
        await pipeline.process_wav(path)
