"""Tests for the L2CAP framing codec.

Frame layout (little-endian per the master plan):
    | u8 type | u8 seq | u16 len | payload[len] |
"""
from __future__ import annotations

import pytest

from assistant_bridge.framing import (
    FRAME_HEADER_LEN,
    MAX_PAYLOAD_LEN,
    FrameType,
    FramingError,
    StreamingFrameParser,
    decode_frame,
    encode_frame,
)


class TestEncodeDecodeRoundTrip:
    def test_audio_frame_round_trip(self):
        payload = b"\x11\x22\x33\x44" * 10
        frame = encode_frame(FrameType.AUDIO, seq=7, payload=payload)
        assert frame[0] == FrameType.AUDIO
        assert frame[1] == 7
        # len is little-endian u16
        assert frame[2] == len(payload) & 0xFF
        assert frame[3] == (len(payload) >> 8) & 0xFF
        assert frame[4:] == payload

        decoded, consumed = decode_frame(frame)
        assert consumed == len(frame)
        assert decoded.type == FrameType.AUDIO
        assert decoded.seq == 7
        assert decoded.payload == payload

    def test_empty_payload_round_trip(self):
        frame = encode_frame(FrameType.CTRL_START_LISTEN, seq=0, payload=b"")
        decoded, consumed = decode_frame(frame)
        assert consumed == FRAME_HEADER_LEN
        assert decoded.payload == b""
        assert decoded.type == FrameType.CTRL_START_LISTEN

    def test_all_frame_types_round_trip(self):
        for ftype in (
            FrameType.AUDIO,
            FrameType.CTRL_STATE,
            FrameType.CTRL_START_LISTEN,
            FrameType.CTRL_STOP_LISTEN,
            FrameType.TEXT_CHUNK,
            FrameType.TEXT_END,
        ):
            payload = b"hello" if ftype != FrameType.TEXT_END else b""
            frame = encode_frame(ftype, seq=3, payload=payload)
            decoded, _ = decode_frame(frame)
            assert decoded.type == ftype
            assert decoded.payload == payload

    def test_seq_wraps_at_u8(self):
        frame = encode_frame(FrameType.AUDIO, seq=255, payload=b"x")
        assert frame[1] == 255
        decoded, _ = decode_frame(frame)
        assert decoded.seq == 255


class TestEncodeErrors:
    def test_seq_out_of_range_rejected(self):
        with pytest.raises(FramingError):
            encode_frame(FrameType.AUDIO, seq=256, payload=b"")
        with pytest.raises(FramingError):
            encode_frame(FrameType.AUDIO, seq=-1, payload=b"")

    def test_payload_too_large_rejected(self):
        with pytest.raises(FramingError):
            encode_frame(FrameType.AUDIO, seq=0, payload=b"\x00" * (MAX_PAYLOAD_LEN + 1))


class TestDecodeErrors:
    def test_short_buffer_raises(self):
        with pytest.raises(FramingError):
            decode_frame(b"\x01\x00\x05")  # header truncated

    def test_incomplete_payload_raises(self):
        # Header says len=5 but only 2 bytes follow
        buf = bytes([FrameType.AUDIO, 0, 5, 0, 0xAA, 0xBB])
        with pytest.raises(FramingError):
            decode_frame(buf)

    def test_unknown_frame_type_raises(self):
        buf = bytes([0xEE, 0, 0, 0])
        with pytest.raises(FramingError):
            decode_frame(buf)


class TestStreamingParser:
    def test_single_complete_frame(self):
        parser = StreamingFrameParser()
        frame = encode_frame(FrameType.AUDIO, 1, b"abc")
        frames = list(parser.feed(frame))
        assert len(frames) == 1
        assert frames[0].payload == b"abc"

    def test_multiple_frames_one_feed(self):
        parser = StreamingFrameParser()
        f1 = encode_frame(FrameType.AUDIO, 1, b"aa")
        f2 = encode_frame(FrameType.TEXT_CHUNK, 2, b"bb")
        frames = list(parser.feed(f1 + f2))
        assert [f.type for f in frames] == [FrameType.AUDIO, FrameType.TEXT_CHUNK]
        assert [f.payload for f in frames] == [b"aa", b"bb"]

    def test_partial_header_buffered(self):
        parser = StreamingFrameParser()
        frame = encode_frame(FrameType.AUDIO, 1, b"xyz")
        # Feed one byte at a time — should accumulate silently until complete.
        out = []
        for i in range(len(frame)):
            out.extend(parser.feed(frame[i : i + 1]))
        assert len(out) == 1
        assert out[0].payload == b"xyz"

    def test_frame_split_across_feeds(self):
        parser = StreamingFrameParser()
        frame = encode_frame(FrameType.AUDIO, 1, b"hello world")
        # Split mid-payload
        part_a = frame[:6]
        part_b = frame[6:]
        assert list(parser.feed(part_a)) == []
        done = list(parser.feed(part_b))
        assert len(done) == 1
        assert done[0].payload == b"hello world"

    def test_trailing_partial_after_complete(self):
        parser = StreamingFrameParser()
        f1 = encode_frame(FrameType.AUDIO, 1, b"a")
        f2 = encode_frame(FrameType.TEXT_CHUNK, 2, b"second")
        chunk = f1 + f2[:3]  # full first + partial second
        first_batch = list(parser.feed(chunk))
        assert len(first_batch) == 1
        second_batch = list(parser.feed(f2[3:]))
        assert len(second_batch) == 1
        assert second_batch[0].payload == b"second"

    def test_unknown_type_surfaces_error(self):
        parser = StreamingFrameParser()
        bad = bytes([0xEE, 0, 0, 0])
        with pytest.raises(FramingError):
            list(parser.feed(bad))

    def test_empty_feed_yields_nothing(self):
        parser = StreamingFrameParser()
        assert list(parser.feed(b"")) == []
