"""Tests for the --transport=uart path.

Covers the end-to-end flow for the Mac -> LLM bridge:

    UART line stream -> PCM_BEGIN/hex/PCM_END parse
        -> AssistantPipeline.process_pcm
        -> mocked whisper transcript
        -> mocked Ollama stream
        -> tokens streamed to stdout

The serial port, faster-whisper model, and Ollama HTTP client are all
mocked — the real tests run with zero hardware and zero network access.
"""
from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import wave
from pathlib import Path
from unittest import mock

import pytest

# conftest puts host/ on sys.path, so this import works from the repo root.
from assistant_bridge import bridge as bridge_mod
from assistant_bridge import pipeline as pipeline_mod
from assistant_bridge.pipeline import (
    AssistantPipeline,
    DryRunLLM,
    OllamaLLM,
    PipelineConfig,
    build_fake_transcriber,
)


# ---------- helpers ---------------------------------------------------------


def _pcm_frame_lines(samples: list[int], sr: int = 16000) -> list[str]:
    payload = b"".join(struct.pack("<h", s) for s in samples)
    hex_payload = payload.hex()
    hex_lines = [hex_payload[i : i + 128] for i in range(0, len(hex_payload), 128)]
    return [
        f"=== PCM_BEGIN samples={len(samples)} sample_rate={sr} channels=1 bits=16 ===",
        *hex_lines,
        "=== PCM_END ===",
    ]


def _default_args(**overrides) -> argparse.Namespace:
    """Build an argparse Namespace matching the bridge's flags."""
    ns = argparse.Namespace(
        transport="uart",
        input=None,
        ollama_url="http://192.168.1.129:11434",
        ollama_model="glm-4.7",
        system_prompt="You are a test assistant.",
        uart_port=None,
        uart_baud=460800,
        uart_port_glob="/dev/cu.usbmodem*",
        dry_run_llm=True,
        fake_transcript="what is the capital of france",
        real=False,
        text_chunk_bytes=32,
        bleak_address=None,
    )
    for k, v in overrides.items():
        setattr(ns, k, v)
    return ns


# ---------- process_pcm -----------------------------------------------------


@pytest.mark.asyncio
async def test_process_pcm_drives_full_pipeline():
    """Raw PCM in -> transcript in -> LLM tokens -> on_token callback fires."""
    outbound: list[bytes] = []

    async def on_outbound(raw: bytes):
        outbound.append(raw)

    cfg = PipelineConfig(dry_run_llm=True, text_chunk_bytes=16)
    pipeline = AssistantPipeline(
        config=cfg,
        opus=mock.MagicMock(),  # UART path skips opus entirely
        transcriber=build_fake_transcriber("hello world"),
        llm=DryRunLLM(reply="Hi there."),
        on_outbound=on_outbound,
    )

    tokens_seen: list[str] = []

    async def on_token(t):
        tokens_seen.append(t)

    pcm = b"\x00\x00" * 1600  # 100 ms of silence
    result = await pipeline.process_pcm(pcm, 16000, on_token=on_token)

    assert result.transcript == "hello world"
    assert result.reply_text == "Hi there."
    assert "".join(tokens_seen) == "Hi there."
    # TEXT_END frame must have been emitted.
    assert len(outbound) >= 1


@pytest.mark.asyncio
async def test_process_pcm_rejects_wrong_sample_rate():
    cfg = PipelineConfig()
    pipeline = AssistantPipeline(
        config=cfg,
        opus=mock.MagicMock(),
        transcriber=build_fake_transcriber("x"),
        llm=DryRunLLM("y"),
    )
    with pytest.raises(ValueError, match="sample rate"):
        await pipeline.process_pcm(b"\x00\x00" * 100, 8000)


@pytest.mark.asyncio
async def test_process_pcm_supports_sync_on_token():
    """on_token may be a plain function or a coroutine — both must work."""
    cfg = PipelineConfig(dry_run_llm=True)
    pipeline = AssistantPipeline(
        config=cfg,
        opus=mock.MagicMock(),
        transcriber=build_fake_transcriber("x"),
        llm=DryRunLLM("a b c"),
    )
    seen: list[str] = []

    def sync_on_token(t):
        seen.append(t)

    await pipeline.process_pcm(b"\x00\x00" * 320, 16000, on_token=sync_on_token)
    assert "".join(seen) == "a b c"


# ---------- Ollama system prompt --------------------------------------------


@pytest.mark.asyncio
async def test_ollama_sends_system_prompt(monkeypatch):
    captured: dict = {}

    class _FakeStream:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def raise_for_status(self):
            pass

        async def aiter_lines(self):
            yield '{"message":{"content":"ok"},"done":true}'

    class _FakeClient:
        def __init__(self, *a, **kw):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def stream(self, method, url, json):
            captured["url"] = url
            captured["body"] = json
            return _FakeStream()

    monkeypatch.setattr(pipeline_mod.httpx, "AsyncClient", _FakeClient)

    llm = OllamaLLM(
        base_url="http://192.168.1.129:11434",
        model="glm-4.7",
        system_prompt="Be terse.",
    )
    tokens = [t async for t in llm.stream_reply("capital of france?")]
    assert tokens == ["ok"]
    assert captured["url"] == "http://192.168.1.129:11434/api/chat"
    assert captured["body"]["model"] == "glm-4.7"
    roles = [m["role"] for m in captured["body"]["messages"]]
    assert roles == ["system", "user"]
    assert captured["body"]["messages"][0]["content"] == "Be terse."
    assert captured["body"]["messages"][1]["content"] == "capital of france?"
    assert captured["body"]["stream"] is True


@pytest.mark.asyncio
async def test_ollama_omits_system_prompt_when_none(monkeypatch):
    captured: dict = {}

    class _FakeStream:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def raise_for_status(self):
            pass

        async def aiter_lines(self):
            yield '{"message":{"content":"x"},"done":true}'

    class _FakeClient:
        def __init__(self, *a, **kw):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def stream(self, method, url, json):
            captured["body"] = json
            return _FakeStream()

    monkeypatch.setattr(pipeline_mod.httpx, "AsyncClient", _FakeClient)
    llm = OllamaLLM(base_url="http://x", model="m", system_prompt=None)
    [t async for t in llm.stream_reply("hi")]
    assert [m["role"] for m in captured["body"]["messages"]] == ["user"]


# ---------- bridge UART transport end-to-end --------------------------------


@pytest.mark.asyncio
async def test_run_uart_dry_run_end_to_end(capsys, monkeypatch):
    """Feed a canned BEGIN/hex/END blob, assert a full pipeline cycle runs
    and the LLM reply hits stdout."""
    samples = [0, 100, -100, 200, -200] * 64  # 320 samples, exactly one 20 ms frame
    canned_lines = _pcm_frame_lines(samples, sr=16000)

    def line_source():
        # Emit real capture lines then stop — mimics EOF on the serial port.
        yield from canned_lines

    # Build args with dry-run LLM so we don't touch the network.
    args = _default_args(dry_run_llm=True, fake_transcript="capital of france")

    # Also override the DryRunLLM reply to a stable string.
    with mock.patch.object(
        bridge_mod,
        "DryRunLLM",
        side_effect=lambda reply=None: DryRunLLM(reply="Paris."),
    ):
        rc = await bridge_mod._run_uart(args, line_source_factory=line_source)

    assert rc == 0
    captured = capsys.readouterr()
    # Reply tokens stream to stdout; transcript/banner go to stderr.
    assert "Paris." in captured.out
    assert "uart transport listening" in captured.err
    assert "captured" in captured.err


@pytest.mark.asyncio
async def test_run_uart_calls_ollama_with_correct_url_and_model(monkeypatch):
    """Real-mode (non-dry-run) UART path: verify pipeline -> Ollama contract."""
    samples = [0] * 320
    canned_lines = _pcm_frame_lines(samples, sr=16000)

    def line_source():
        yield from canned_lines

    seen: dict = {}

    class _FakeStream:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def raise_for_status(self):
            pass

        async def aiter_lines(self):
            for line in [
                '{"message":{"content":"Par"}}',
                '{"message":{"content":"is."},"done":true}',
            ]:
                yield line

    class _FakeClient:
        def __init__(self, *a, **kw):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *a):
            return None

        def stream(self, method, url, json):
            seen["url"] = url
            seen["model"] = json["model"]
            seen["messages"] = json["messages"]
            return _FakeStream()

    monkeypatch.setattr(pipeline_mod.httpx, "AsyncClient", _FakeClient)

    args = _default_args(
        dry_run_llm=False,
        ollama_url="http://192.168.1.129:11434",
        ollama_model="glm-4.7",
    )
    rc = await bridge_mod._run_uart(args, line_source_factory=line_source)
    assert rc == 0
    assert seen["url"] == "http://192.168.1.129:11434/api/chat"
    assert seen["model"] == "glm-4.7"
    assert seen["messages"][0]["role"] == "system"
    assert "voice assistant" in seen["messages"][0]["content"].lower() or (
        seen["messages"][0]["content"] == args.system_prompt
    )


@pytest.mark.asyncio
async def test_run_uart_errors_when_no_port_match(capsys):
    """Without a matching serial port, the transport exits non-zero."""
    args = _default_args(uart_port=None, uart_port_glob="/definitely/no/such/*")
    rc = await bridge_mod._run_uart(args)  # no injection, real resolve
    assert rc == 2
    err = capsys.readouterr().err
    assert "no serial device matched" in err


def test_build_parser_accepts_uart_transport():
    """Regression: --transport=uart must be a valid choice."""
    p = bridge_mod._build_parser()
    args = p.parse_args(
        [
            "--transport=uart",
            "--uart-port=/dev/fake",
            "--uart-baud=921600",
            "--ollama-url=http://example:11434",
            "--ollama-model=glm-4.7",
            "--dry-run-llm",
        ]
    )
    assert args.transport == "uart"
    assert args.uart_port == "/dev/fake"
    assert args.uart_baud == 921600
    assert args.dry_run_llm is True


@pytest.mark.asyncio
async def test_resolve_uart_port_prefers_explicit_flag():
    args = _default_args(uart_port="/dev/explicit")
    assert bridge_mod._resolve_uart_port(args) == "/dev/explicit"


@pytest.mark.asyncio
async def test_resolve_uart_port_globs_when_unset(monkeypatch):
    args = _default_args(uart_port=None, uart_port_glob="/dev/cu.usbmodem*")
    from audio_capture import uart_protocol

    monkeypatch.setattr(
        uart_protocol.glob, "glob", lambda pat: ["/dev/cu.usbmodemFAKE"]
    )
    assert bridge_mod._resolve_uart_port(args) == "/dev/cu.usbmodemFAKE"


@pytest.mark.asyncio
async def test_run_uart_handles_multiple_captures_in_sequence(capsys, monkeypatch):
    """The UART listener is a prompt-loop: two captures -> two pipeline runs."""
    first = _pcm_frame_lines([0] * 320)
    second = _pcm_frame_lines([1] * 320)

    def line_source():
        yield from first
        yield from second

    # Count pipeline runs by wrapping process_pcm on the AssistantPipeline
    # class itself — _run_uart constructs its own instance.
    run_count = {"n": 0}

    orig = AssistantPipeline.process_pcm

    async def counting_process_pcm(self, pcm, sr, *, on_token=None):
        run_count["n"] += 1
        return await orig(self, pcm, sr, on_token=on_token)

    monkeypatch.setattr(AssistantPipeline, "process_pcm", counting_process_pcm)

    args = _default_args(dry_run_llm=True)
    rc = await bridge_mod._run_uart(args, line_source_factory=line_source)
    assert rc == 0
    assert run_count["n"] == 2


def test_make_uart_line_source_delegates_to_serial_lines(monkeypatch):
    """`_make_uart_line_source` must hand off to audio_capture._serial_lines."""
    from audio_capture import uart_protocol

    called: dict = {}

    def fake_serial_lines(port, baud):
        called["port"] = port
        called["baud"] = baud
        yield "line1"
        yield "line2"

    monkeypatch.setattr(uart_protocol, "_serial_lines", fake_serial_lines)
    lines = list(bridge_mod._make_uart_line_source("/dev/fake", 460800))
    assert called == {"port": "/dev/fake", "baud": 460800}
    assert lines == ["line1", "line2"]


@pytest.mark.asyncio
async def test_run_uart_resolves_port_when_no_factory(monkeypatch, capsys):
    """No ``line_source_factory`` -> resolve port + call _make_uart_line_source."""
    from audio_capture import uart_protocol

    monkeypatch.setattr(
        uart_protocol.glob, "glob", lambda pat: ["/dev/cu.usbmodemXYZ"]
    )

    canned = _pcm_frame_lines([0] * 320)

    def fake_make_source(port, baud):
        assert port == "/dev/cu.usbmodemXYZ"
        assert baud == 460800
        yield from canned

    monkeypatch.setattr(bridge_mod, "_make_uart_line_source", fake_make_source)

    args = _default_args(dry_run_llm=True)
    rc = await bridge_mod._run_uart(args)
    assert rc == 0
    err = capsys.readouterr().err
    assert "/dev/cu.usbmodemXYZ @ 460800 baud" in err


@pytest.mark.asyncio
async def test_run_uart_handles_keyboard_interrupt(capsys):
    """Ctrl-C during the capture loop exits cleanly with rc=0."""

    def line_source():
        def _gen():
            raise KeyboardInterrupt
            yield  # unreachable — makes this a generator
        return _gen()

    args = _default_args(dry_run_llm=True)
    rc = await bridge_mod._run_uart(args, line_source_factory=line_source)
    assert rc == 0
    assert "stopped" in capsys.readouterr().err
