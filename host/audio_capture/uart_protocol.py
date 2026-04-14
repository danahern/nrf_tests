"""Reusable PCM_BEGIN / hex / PCM_END UART protocol parser.

The PSE84 Phase 2 firmware emits PCM captures over UART as ASCII-framed
hex blocks:

    === PCM_BEGIN samples=<N> sample_rate=<SR> channels=<C> bits=<B> ===
    <hex payload, 64 bytes per line>
    === PCM_END ===

This module factors the parser out of ``receive_pcm.py`` so other host
tools (the assistant bridge in particular) can consume the same stream
without duplicating protocol knowledge.

Pure stdlib. ``pyserial`` is imported lazily inside
:func:`iter_captures_from_serial` so unit tests that drive the parser
with fake line iterables need no native dependency.
"""
from __future__ import annotations

import glob
import re
import struct
from dataclasses import dataclass
from typing import Iterable, Iterator, Optional


BEGIN_RE = re.compile(
    r"=== PCM_BEGIN samples=(?P<samples>\d+) sample_rate=(?P<sr>\d+) "
    r"channels=(?P<ch>\d+) bits=(?P<bits>\d+) ==="
)
END_MARKER = "=== PCM_END ==="
HEX_RE = re.compile(r"^[0-9a-fA-F]+$")

OPUS_BEGIN_RE = re.compile(
    r"=== OPUS_BEGIN frames=(?P<frames>\d+) frame_samples=(?P<fs>\d+) "
    r"sample_rate=(?P<sr>\d+) bitrate=(?P<br>\d+) ==="
)
OPUS_END_MARKER = "=== OPUS_END ==="


@dataclass
class OpusBlock:
    """A captured Opus stream: per-frame (u16-LE length + opus bytes)."""

    frames: int
    frame_samples: int
    sample_rate: int
    bitrate: int
    packets: list[bytes]  # one entry per encoded frame, raw opus bytes

    @property
    def total_bytes(self) -> int:
        return sum(len(p) for p in self.packets)


def parse_opus_block(lines: Iterable[str]) -> Optional[OpusBlock]:
    """Parse a single complete OPUS_BEGIN / ... / OPUS_END block."""
    it = iter(lines)
    header: Optional[re.Match[str]] = None
    for line in it:
        m = OPUS_BEGIN_RE.search(line)
        if m:
            header = m
            break
    if header is None:
        return None

    frames_count = int(header.group("frames"))
    fs = int(header.group("fs"))
    sr = int(header.group("sr"))
    br = int(header.group("br"))

    hex_chunks: list[str] = []
    found_end = False
    for line in it:
        stripped = line.strip()
        if stripped == OPUS_END_MARKER:
            found_end = True
            break
        if HEX_RE.match(stripped):
            hex_chunks.append(stripped)
    if not found_end:
        raise ValueError("OPUS_BEGIN seen but no matching OPUS_END")

    raw = bytes.fromhex("".join(hex_chunks))
    # The firmware interleaves (u16 LE length prefix, opus bytes) per frame.
    packets: list[bytes] = []
    off = 0
    while off < len(raw):
        if off + 2 > len(raw):
            raise ValueError("truncated OPUS length prefix")
        n = raw[off] | (raw[off + 1] << 8)
        off += 2
        if off + n > len(raw):
            raise ValueError(
                f"truncated OPUS payload (need {n}, have {len(raw) - off})"
            )
        packets.append(raw[off:off + n])
        off += n
    if len(packets) != frames_count:
        raise ValueError(
            f"expected {frames_count} Opus packets, decoded {len(packets)}"
        )
    return OpusBlock(
        frames=frames_count,
        frame_samples=fs,
        sample_rate=sr,
        bitrate=br,
        packets=packets,
    )


@dataclass
class PcmFrame:
    """A captured PCM frame from the UART protocol."""

    samples: int
    sample_rate: int
    channels: int
    bits: int
    pcm: bytes  # little-endian signed int16 interleaved

    @property
    def duration_ms(self) -> float:
        return 1000.0 * self.samples / self.sample_rate

    def peak(self) -> int:
        """Absolute peak amplitude (signed 16-bit assumed)."""
        peak = 0
        for (s,) in struct.iter_unpack("<h", self.pcm):
            a = -s if s < 0 else s
            if a > peak:
                peak = a
        return peak


def parse_frame(lines: Iterable[str]) -> Optional[PcmFrame]:
    """Parse a single complete frame from an iterable of stripped lines.

    Returns ``None`` if no ``PCM_BEGIN`` header is found. Raises
    :class:`ValueError` on a malformed frame (missing end marker,
    wrong hex length).
    """
    it = iter(lines)
    header: Optional[re.Match[str]] = None
    for line in it:
        m = BEGIN_RE.search(line)
        if m:
            header = m
            break
    if header is None:
        return None

    samples = int(header.group("samples"))
    sr = int(header.group("sr"))
    ch = int(header.group("ch"))
    bits = int(header.group("bits"))
    expected_hex_chars = 2 * samples * ch * (bits // 8)

    hex_chunks: list[str] = []
    found_end = False
    for line in it:
        stripped = line.strip()
        if stripped == END_MARKER:
            found_end = True
            break
        if HEX_RE.match(stripped):
            hex_chunks.append(stripped)
    if not found_end:
        raise ValueError("PCM_BEGIN seen but no matching PCM_END")

    hex_payload = "".join(hex_chunks)
    if len(hex_payload) != expected_hex_chars:
        raise ValueError(
            f"expected {expected_hex_chars} hex chars, got {len(hex_payload)}"
        )
    pcm = bytes.fromhex(hex_payload)
    return PcmFrame(samples=samples, sample_rate=sr, channels=ch, bits=bits, pcm=pcm)


def iter_captures(lines: Iterable[str]) -> Iterator[PcmFrame]:
    """Yield every complete :class:`PcmFrame` found in ``lines``.

    ``lines`` is any iterable of already-stripped (no CRLF) strings —
    typically wrapping ``serial.Serial.readline()`` decoded output, or a
    canned list in tests. Non-frame lines (HW log noise) are ignored.
    Malformed frames are skipped with an ``on_error`` callback path left
    for higher-level callers — we raise here and expect them to catch.
    """
    buf: list[str] = []
    in_frame = False
    for line in lines:
        if BEGIN_RE.search(line):
            buf = [line]
            in_frame = True
            continue
        if not in_frame:
            continue
        buf.append(line)
        if line.strip() == END_MARKER:
            frame = parse_frame(buf)
            in_frame = False
            buf = []
            if frame is not None:
                yield frame


def iter_captures_mixed(lines: Iterable[str],
                        log_passthrough=None) -> Iterator[object]:
    """Yield PcmFrame or OpusBlock objects in stream order.

    The Phase 3 firmware emits PCM_BEGIN/PCM_END first and then (when
    CONFIG_APP_AUDIO_EMIT_OPUS is set) OPUS_BEGIN/OPUS_END. Callers that
    want both use this instead of :func:`iter_captures`.

    ``log_passthrough`` is an optional callable(str) invoked on every
    non-payload, non-marker line — used by ``receive_pcm.py`` to surface
    Zephyr LOG_INF lines (``button press``, ``capture START/STOP``,
    heartbeat, etc.) which would otherwise be silently swallowed.
    """
    buf: list[str] = []
    mode: Optional[str] = None  # "pcm" or "opus"
    for line in lines:
        stripped = line.strip()
        if BEGIN_RE.search(line):
            buf = [line]
            mode = "pcm"
            continue
        if OPUS_BEGIN_RE.search(line):
            buf = [line]
            mode = "opus"
            continue
        if mode is None:
            # Neither payload nor framing — hand to log passthrough so
            # the firmware's diagnostic log lines reach stdout.
            if log_passthrough is not None and stripped:
                if not HEX_RE.match(stripped):
                    log_passthrough(stripped)
            continue
        buf.append(line)
        if mode == "pcm" and stripped == END_MARKER:
            frame = parse_frame(buf)
            mode = None
            buf = []
            if frame is not None:
                yield frame
        elif mode == "opus" and stripped == OPUS_END_MARKER:
            block = parse_opus_block(buf)
            mode = None
            buf = []
            if block is not None:
                yield block


def _serial_lines(port: str, baud: int) -> Iterator[str]:
    """Readline loop over a pyserial port, decoded + stripped."""
    import serial  # local import — tests don't need pyserial installed

    with serial.Serial(port, baudrate=baud, timeout=1) as ser:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            try:
                yield raw.decode("ascii", errors="replace").rstrip("\r\n")
            except Exception:
                continue


def iter_captures_from_serial(port: str, baud: int) -> Iterator[PcmFrame]:
    """Open ``port`` at ``baud`` and yield :class:`PcmFrame` captures forever."""
    yield from iter_captures(_serial_lines(port, baud))


def find_serial_port(pattern: str = "/dev/cu.usbmodem*") -> Optional[str]:
    """Return the first serial port matching ``pattern``, or ``None``."""
    matches = sorted(glob.glob(pattern))
    return matches[0] if matches else None
