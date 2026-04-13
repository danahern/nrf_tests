"""macOS Core Audio input for the assistant bridge.

Provides two objects consumed by ``bridge._run_mac_mic``:

* :class:`SoundDeviceCapture` — thin wrapper around ``sounddevice.InputStream``
  that yields one ``bytes`` blob per utterance (space-toggle window).

* :class:`StdinSpaceToggle` — reads SPACE key presses from the terminal in
  cbreak mode and delivers asyncio events. SPACE toggles the recording
  state; a configurable timeout bounds each utterance.

Both are isolated here so tests can swap in deterministic fakes without
importing ``sounddevice`` (which requires PortAudio native libs).
"""
from __future__ import annotations

import asyncio
import queue
import sys
import termios
import tty
from typing import AsyncIterator, Optional


class StdinSpaceToggle:
    """Reads single keystrokes from stdin in cbreak mode, raising a
    toggle event each time SPACE is pressed. Ctrl-C / ESC closes the source.
    """

    def __init__(self) -> None:
        self._loop = asyncio.get_event_loop()
        self._queue: asyncio.Queue[str] = asyncio.Queue()
        self._fd: Optional[int] = None
        self._old_attrs = None
        self._reader_installed = False
        try:
            self._fd = sys.stdin.fileno()
            self._old_attrs = termios.tcgetattr(self._fd)
            tty.setcbreak(self._fd)
            self._loop.add_reader(self._fd, self._on_readable)
            self._reader_installed = True
        except (termios.error, OSError, ValueError):
            # Not a TTY (e.g., pytest captured stdin) — degrade gracefully.
            self._fd = None

    def _on_readable(self) -> None:
        assert self._fd is not None
        ch = sys.stdin.read(1)
        if not ch:
            return
        if ch == " ":
            self._queue.put_nowait("toggle")
        elif ch in ("\x1b", "\x03"):  # ESC or Ctrl-C
            self._queue.put_nowait("quit")

    async def next_event(self) -> str:
        """Await the next key event: 'toggle' or 'quit'."""
        return await self._queue.get()

    def close(self) -> None:
        if self._reader_installed and self._fd is not None:
            try:
                self._loop.remove_reader(self._fd)
            except Exception:
                pass
            self._reader_installed = False
        if self._fd is not None and self._old_attrs is not None:
            try:
                termios.tcsetattr(self._fd, termios.TCSADRAIN, self._old_attrs)
            except Exception:
                pass
            self._old_attrs = None


class SoundDeviceCapture:
    """Captures PCM from the default (or named) macOS input device.

    ``iter_utterances`` yields ``bytes`` (s16le interleaved if stereo;
    mono here) for each space-toggle window.
    """

    def __init__(
        self,
        *,
        device: Optional[str] = None,
        sample_rate: int = 16000,
        max_seconds: float = 8.0,
        block_ms: int = 20,
    ) -> None:
        self._device = device
        self._sample_rate = sample_rate
        self._max_seconds = max_seconds
        self._block_frames = int(sample_rate * block_ms / 1000)
        self._stream = None
        self._blocks: queue.Queue[bytes] = queue.Queue()

    def _ensure_stream(self):
        if self._stream is not None:
            return
        import sounddevice as sd

        def _cb(indata, frames, time, status):
            if status:
                print(f"[mac-mic sd status] {status}", file=sys.stderr)
            self._blocks.put(bytes(indata))

        kwargs = dict(
            samplerate=self._sample_rate,
            channels=1,
            dtype="int16",
            blocksize=self._block_frames,
            callback=_cb,
        )
        if self._device is not None:
            kwargs["device"] = self._device
        self._stream = sd.InputStream(**kwargs)
        self._stream.start()

    def _drain(self) -> bytes:
        out = bytearray()
        while True:
            try:
                out.extend(self._blocks.get_nowait())
            except queue.Empty:
                return bytes(out)

    async def iter_utterances(
        self, key_source: "StdinSpaceToggle"
    ) -> AsyncIterator[bytes]:
        """Yield one PCM blob per SPACE-toggle window.

        Flow: SPACE → start; SPACE again OR timeout → stop + yield.
        """
        loop = asyncio.get_event_loop()
        self._ensure_stream()

        while True:
            ev = await key_source.next_event()
            if ev == "quit":
                return
            if ev != "toggle":
                continue
            # Starting: drop any blocks captured while idle.
            while not self._blocks.empty():
                self._blocks.get_nowait()
            print("[mac-mic] recording…", file=sys.stderr)

            pcm = bytearray()
            start = loop.time()
            stopped = False
            while not stopped:
                # Wait up to 50ms for either a key event or the next audio block
                try:
                    ev = await asyncio.wait_for(key_source.next_event(), 0.05)
                    if ev == "toggle":
                        stopped = True
                    elif ev == "quit":
                        pcm.extend(self._drain())
                        if pcm:
                            yield bytes(pcm)
                        return
                except asyncio.TimeoutError:
                    pass
                pcm.extend(self._drain())
                if loop.time() - start >= self._max_seconds:
                    print(
                        f"[mac-mic] max {self._max_seconds:.1f}s reached, stopping",
                        file=sys.stderr,
                    )
                    stopped = True

            if pcm:
                yield bytes(pcm)

    async def aclose(self) -> None:
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            except Exception:
                pass
            self._stream = None
