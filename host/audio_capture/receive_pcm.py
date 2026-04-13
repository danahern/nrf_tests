#!/usr/bin/env python3
"""Receive PCM hex dumps from pse84_assistant over UART and write WAV.

Looks for frames of the form:

    === PCM_BEGIN samples=<N> sample_rate=<SR> channels=<C> bits=<B> ===
    <hex payload, 2*N*B/8 chars, 64 chars per line>
    === PCM_END ===

On each complete frame, decodes the hex payload, writes a WAV file to
captures/<timestamp>.wav, prints a summary, and autoplays via afplay
(macOS) / aplay (Linux).

Uses only the stdlib (+ pyserial at runtime for the HW path). The
codec/hex/WAV path is pure stdlib so the unit tests can exercise it
without any serial hardware.

Requires: pip install pyserial
"""

from __future__ import annotations

import argparse
import glob
import os
import platform
import re
import struct
import subprocess
import sys
import time
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Optional

BEGIN_RE = re.compile(
    r"=== PCM_BEGIN samples=(?P<samples>\d+) sample_rate=(?P<sr>\d+) "
    r"channels=(?P<ch>\d+) bits=(?P<bits>\d+) ==="
)
END_MARKER = "=== PCM_END ==="
HEX_RE = re.compile(r"^[0-9a-fA-F]+$")


@dataclass
class PcmFrame:
    """A captured PCM frame."""

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
        # struct iter is faster than decoding one at a time.
        for (s,) in struct.iter_unpack("<h", self.pcm):
            a = -s if s < 0 else s
            if a > peak:
                peak = a
        return peak


def parse_frame(lines: Iterable[str]) -> Optional[PcmFrame]:
    """Parse a complete frame from an iterable of lines.

    Lines must NOT include trailing newlines. Returns None if no valid
    frame is found; raises ValueError on a malformed frame.
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
        # Tolerate interleaved log noise on the same UART — skip lines
        # that aren't pure hex.
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


def write_wav(frame: PcmFrame, path: Path) -> None:
    """Write a PCM frame to a canonical PCM-WAV file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(frame.channels)
        w.setsampwidth(frame.bits // 8)
        w.setframerate(frame.sample_rate)
        w.writeframes(frame.pcm)


def autoplay(path: Path) -> None:
    """Best-effort autoplay. Never raises — we still want the file."""
    system = platform.system()
    if system == "Darwin":
        cmd = ["afplay", str(path)]
    elif system == "Linux":
        cmd = ["aplay", str(path)]
    else:
        print(f"(autoplay skipped: unsupported platform {system})")
        return
    try:
        subprocess.run(cmd, check=False)
    except FileNotFoundError:
        print(f"(autoplay skipped: {cmd[0]} not installed)")


def find_serial_port(pattern: str) -> Optional[str]:
    matches = sorted(glob.glob(pattern))
    return matches[0] if matches else None


def stream_frames_from_serial(port: str, baud: int) -> Iterator[PcmFrame]:
    """Generator: yield one PcmFrame per captured burst."""
    import serial  # local import so the unit tests don't need pyserial

    with serial.Serial(port, baudrate=baud, timeout=1) as ser:
        print(f"listening on {port} @ {baud} baud (Ctrl-C to stop)")
        buf: list[str] = []
        in_frame = False
        while True:
            raw = ser.readline()
            if not raw:
                continue
            try:
                line = raw.decode("ascii", errors="replace").rstrip("\r\n")
            except Exception:
                continue
            if BEGIN_RE.search(line):
                buf = [line]
                in_frame = True
                continue
            if in_frame:
                buf.append(line)
                if line.strip() == END_MARKER:
                    try:
                        frame = parse_frame(buf)
                    except ValueError as exc:
                        print(f"frame parse error: {exc}")
                        in_frame = False
                        buf = []
                        continue
                    in_frame = False
                    buf = []
                    if frame is not None:
                        yield frame
            else:
                # Print stray output so the user can still see HW logs.
                print(f"  | {line}")


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--port",
        default=None,
        help="serial device (default: auto-glob /dev/cu.usbmodem*)",
    )
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).parent / "captures",
        help="directory to write WAV files into",
    )
    p.add_argument("--no-play", action="store_true")
    args = p.parse_args(argv)

    port = args.port or find_serial_port("/dev/cu.usbmodem*")
    if not port:
        print("ERROR: no /dev/cu.usbmodem* device found", file=sys.stderr)
        return 2

    try:
        for frame in stream_frames_from_serial(port, args.baud):
            ts = time.strftime("%Y%m%d_%H%M%S")
            out = args.out_dir / f"{ts}.wav"
            write_wav(frame, out)
            peak = frame.peak()
            print(
                f"captured {frame.samples} samples "
                f"({frame.duration_ms:.0f} ms), peak={peak}, wav={out}"
            )
            if not args.no_play:
                autoplay(out)
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
