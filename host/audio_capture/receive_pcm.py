#!/usr/bin/env python3
"""Receive PCM hex dumps from pse84_assistant over UART and write WAV.

Looks for frames of the form:

    === PCM_BEGIN samples=<N> sample_rate=<SR> channels=<C> bits=<B> ===
    <hex payload, 2*N*B/8 chars, 64 chars per line>
    === PCM_END ===

On each complete frame, decodes the hex payload, writes a WAV file to
captures/<timestamp>.wav, prints a summary, and autoplays via afplay
(macOS) / aplay (Linux).

The parser itself lives in :mod:`uart_protocol` so other host tools
(the assistant bridge) can reuse it. This module is a thin CLI on top.

Requires: pip install pyserial
"""

from __future__ import annotations

import argparse
import platform
import subprocess
import sys
import time
import wave
from pathlib import Path
from typing import Iterator, Optional

from uart_protocol import (
    BEGIN_RE,
    END_MARKER,
    HEX_RE,
    PcmFrame,
    find_serial_port,
    iter_captures,
    iter_captures_from_serial,
    parse_frame,
)


# Re-exports for the existing test suite / callers that historically
# reached into receive_pcm for these names.
__all__ = [
    "BEGIN_RE",
    "END_MARKER",
    "HEX_RE",
    "PcmFrame",
    "parse_frame",
    "write_wav",
    "autoplay",
    "find_serial_port",
    "stream_frames_from_serial",
    "main",
]


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


def stream_frames_from_serial(port: str, baud: int) -> Iterator[PcmFrame]:
    """Generator: yield one :class:`PcmFrame` per captured burst.

    Thin wrapper around :func:`uart_protocol.iter_captures_from_serial`
    that also prints a connection banner, matching the previous CLI
    behavior.
    """
    print(f"listening on {port} @ {baud} baud (Ctrl-C to stop)")
    yield from iter_captures_from_serial(port, baud)


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--port",
        default=None,
        help="serial device (default: auto-glob /dev/cu.usbmodem*)",
    )
    p.add_argument("--baud", type=int, default=460800)
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
