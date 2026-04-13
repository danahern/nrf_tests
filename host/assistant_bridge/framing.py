"""L2CAP CoC framing codec for the PSE84 voice-assistant bridge.

Wire format (little-endian):

    | u8 type | u8 seq | u16 len | payload[len] |

The frame types mirror the master plan and are authoritative end-to-end:
both the M33 BLE shim and the macOS host speak this framing directly —
the M33 does not interpret payloads.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import Iterator


FRAME_HEADER_LEN = 4
MAX_PAYLOAD_LEN = 0xFFFF  # u16
_HEADER_FMT = "<BBH"


class FrameType(IntEnum):
    AUDIO = 0x01  # Opus 20 ms frame
    CTRL_STATE = 0x10  # u8 state enum
    CTRL_START_LISTEN = 0x11
    CTRL_STOP_LISTEN = 0x12
    TEXT_CHUNK = 0x20  # UTF-8 partial text
    TEXT_END = 0x21  # end-of-response sentinel


_KNOWN_TYPES = frozenset(int(t) for t in FrameType)


class FramingError(ValueError):
    """Raised on malformed or unknown frames."""


@dataclass(frozen=True)
class Frame:
    type: FrameType
    seq: int
    payload: bytes


def encode_frame(ftype: FrameType | int, seq: int, payload: bytes) -> bytes:
    """Encode a single frame. Raises FramingError on out-of-range values."""
    if not (0 <= seq <= 0xFF):
        raise FramingError(f"seq out of u8 range: {seq}")
    if len(payload) > MAX_PAYLOAD_LEN:
        raise FramingError(
            f"payload too large: {len(payload)} > {MAX_PAYLOAD_LEN}"
        )
    header = struct.pack(_HEADER_FMT, int(ftype), seq, len(payload))
    return header + payload


def decode_frame(buf: bytes) -> tuple[Frame, int]:
    """Decode one frame from the start of ``buf``.

    Returns ``(frame, bytes_consumed)``. Raises ``FramingError`` if the buffer
    is short, the payload length does not match, or the frame type is unknown.
    """
    if len(buf) < FRAME_HEADER_LEN:
        raise FramingError(
            f"buffer shorter than header: {len(buf)} < {FRAME_HEADER_LEN}"
        )
    ftype_raw, seq, plen = struct.unpack_from(_HEADER_FMT, buf, 0)
    total = FRAME_HEADER_LEN + plen
    if len(buf) < total:
        raise FramingError(
            f"buffer shorter than declared payload: {len(buf)} < {total}"
        )
    if ftype_raw not in _KNOWN_TYPES:
        raise FramingError(f"unknown frame type: 0x{ftype_raw:02x}")
    payload = bytes(buf[FRAME_HEADER_LEN:total])
    return Frame(FrameType(ftype_raw), seq, payload), total


class StreamingFrameParser:
    """Incremental frame parser for byte streams (L2CAP SDU aggregator).

    BLE SDUs do not guarantee per-frame alignment, so callers push arbitrary
    chunks via :meth:`feed` and iterate over the completed :class:`Frame`
    instances that fall out.
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> Iterator[Frame]:
        if chunk:
            self._buf.extend(chunk)
        while True:
            if len(self._buf) < FRAME_HEADER_LEN:
                return
            ftype_raw, seq, plen = struct.unpack_from(_HEADER_FMT, self._buf, 0)
            total = FRAME_HEADER_LEN + plen
            if len(self._buf) < total:
                return
            if ftype_raw not in _KNOWN_TYPES:
                # Drop the offending byte so the exception is recoverable by a
                # caller that resyncs, but still surface the error.
                del self._buf[0]
                raise FramingError(f"unknown frame type: 0x{ftype_raw:02x}")
            payload = bytes(self._buf[FRAME_HEADER_LEN:total])
            del self._buf[:total]
            yield Frame(FrameType(ftype_raw), seq, payload)
