"""Tests for the mac-mic transport glue. Uses stubbed capture + key sources
so nothing touches sounddevice or termios."""
from __future__ import annotations

import asyncio
import sys
from types import SimpleNamespace

import pytest

from assistant_bridge import bridge as bridge_mod


class _FakeKeySource:
    def __init__(self, events):
        self._q = asyncio.Queue()
        for ev in events:
            self._q.put_nowait(ev)

    async def next_event(self):
        return await self._q.get()

    def close(self):
        pass


class _FakeCapture:
    """Emits one canned PCM blob per invocation, then stops."""

    def __init__(self, utterances):
        self._utterances = list(utterances)
        self.closed = False

    async def iter_utterances(self, key_source):
        for u in self._utterances:
            # Consume one "toggle" to mark the start, one to stop.
            await key_source.next_event()  # start
            # key_source may or may not deliver a stop; ignore.
            yield u

    async def aclose(self):
        self.closed = True


@pytest.mark.asyncio
async def test_mac_mic_processes_one_utterance_via_pipeline():
    args = bridge_mod._build_parser().parse_args(
        [
            "--transport=mac-mic",
            "--dry-run-llm",
            "--fake-transcript=hello world",
            "--mac-mic-sample-rate=16000",
        ]
    )
    # 16 kHz mono s16 * 1.0 s = 32000 bytes
    pcm = b"\x00\x01" * 16000
    fake_capture = _FakeCapture([pcm])
    fake_keys = _FakeKeySource(["toggle", "quit"])

    rc = await bridge_mod._run_mac_mic(
        args,
        capture_factory=lambda: fake_capture,
        input_factory=lambda: fake_keys,
    )
    assert rc == 0
    assert fake_capture.closed is True


@pytest.mark.asyncio
async def test_mac_mic_skips_short_clips():
    args = bridge_mod._build_parser().parse_args(
        [
            "--transport=mac-mic",
            "--dry-run-llm",
            "--mac-mic-sample-rate=16000",
        ]
    )
    too_short = b"\x00\x01" * 100  # 6.25 ms
    fake_capture = _FakeCapture([too_short])
    fake_keys = _FakeKeySource(["toggle", "quit"])

    # Should not raise and should return cleanly even though the clip
    # is below the 100 ms threshold.
    rc = await bridge_mod._run_mac_mic(
        args,
        capture_factory=lambda: fake_capture,
        input_factory=lambda: fake_keys,
    )
    assert rc == 0
