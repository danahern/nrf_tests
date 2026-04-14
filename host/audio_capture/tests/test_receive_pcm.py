"""Unit tests for the receive_pcm.py parser + WAV writer.

Covers the hex->PCM->WAV path end-to-end without any serial hardware.
Run from the repo root with:

    python3 -m pytest host/audio_capture/tests

or standalone:

    python3 host/audio_capture/tests/test_receive_pcm.py
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path

# Add parent dir to sys.path so we can import receive_pcm without packaging.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import receive_pcm  # noqa: E402


def _make_frame_lines(samples: list[int], sr: int = 16000) -> list[str]:
    """Build the exact UART lines the firmware would emit for `samples`."""
    payload = b"".join(struct.pack("<h", s) for s in samples)
    hex_payload = payload.hex()
    # Firmware wraps at 64 bytes = 128 hex chars per line.
    hex_lines = [
        hex_payload[i : i + 128] for i in range(0, len(hex_payload), 128)
    ]
    return [
        f"=== PCM_BEGIN samples={len(samples)} sample_rate={sr} channels=1 bits=16 ===",
        *hex_lines,
        "=== PCM_END ===",
    ]


class TestParseFrame(unittest.TestCase):
    def test_round_trip_known_samples(self):
        samples = [0, 1000, -1000, 32767, -32768, 12345]
        lines = _make_frame_lines(samples)
        frame = receive_pcm.parse_frame(lines)
        self.assertIsNotNone(frame)
        assert frame is not None  # for type checkers
        self.assertEqual(frame.samples, len(samples))
        self.assertEqual(frame.sample_rate, 16000)
        self.assertEqual(frame.channels, 1)
        self.assertEqual(frame.bits, 16)
        decoded = list(struct.iter_unpack("<h", frame.pcm))
        self.assertEqual([s for (s,) in decoded], samples)

    def test_tolerates_log_noise(self):
        samples = [100, 200, 300]
        lines = _make_frame_lines(samples)
        # Interleave a log line after BEGIN that isn't hex — the parser
        # should skip it and still reconstruct the payload.
        lines.insert(1, "[00:00:01.123] <inf> audio: capture active")
        frame = receive_pcm.parse_frame(lines)
        assert frame is not None
        self.assertEqual(frame.samples, 3)

    def test_no_begin_returns_none(self):
        self.assertIsNone(receive_pcm.parse_frame(["random", "garbage"]))

    def test_missing_end_raises(self):
        samples = [1, 2, 3]
        lines = _make_frame_lines(samples)
        lines.pop()  # drop PCM_END
        with self.assertRaises(ValueError):
            receive_pcm.parse_frame(lines)

    def test_short_payload_is_zero_padded(self):
        # Parser is now lenient: rather than raise, short payloads are
        # zero-padded so the bridge stays alive through occasional UART
        # byte loss on the PSE84 hex-dump burst.
        samples = [1, 2, 3]
        lines = _make_frame_lines(samples)
        lines[1] = lines[1][:-2]  # drop one byte's worth of hex
        frame = receive_pcm.parse_frame(lines)
        assert frame is not None
        assert frame.samples == 3
        # Last sample gets its low byte zero'd out by the pad; sample
        # count is preserved so downstream code (WAV writer, Whisper)
        # sees a correctly-sized buffer.
        assert len(frame.pcm) == 3 * 2

    def test_truncates_overlong_payload(self):
        samples = [1, 2, 3]
        lines = _make_frame_lines(samples)
        lines[1] = lines[1] + "deadbeef"  # extra hex
        frame = receive_pcm.parse_frame(lines)
        assert frame is not None
        assert len(frame.pcm) == 3 * 2


class TestPeak(unittest.TestCase):
    def test_peak_absolute(self):
        samples = [0, -30000, 100, -32768, 32767]
        frame = receive_pcm.parse_frame(_make_frame_lines(samples))
        assert frame is not None
        self.assertEqual(frame.peak(), 32768)

    def test_peak_zero(self):
        frame = receive_pcm.parse_frame(_make_frame_lines([0, 0, 0]))
        assert frame is not None
        self.assertEqual(frame.peak(), 0)


class TestWriteWav(unittest.TestCase):
    def test_write_wav_is_readable(self):
        samples = [i - 1000 for i in range(2000)]
        frame = receive_pcm.parse_frame(_make_frame_lines(samples, sr=16000))
        assert frame is not None
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "test.wav"
            receive_pcm.write_wav(frame, out)
            self.assertTrue(out.exists())
            with wave.open(str(out), "rb") as r:
                self.assertEqual(r.getnchannels(), 1)
                self.assertEqual(r.getsampwidth(), 2)
                self.assertEqual(r.getframerate(), 16000)
                self.assertEqual(r.getnframes(), len(samples))
                raw = r.readframes(r.getnframes())
                decoded = [s for (s,) in struct.iter_unpack("<h", raw)]
                self.assertEqual(decoded, samples)


class TestDuration(unittest.TestCase):
    def test_duration_ms(self):
        samples = [0] * 1600
        frame = receive_pcm.parse_frame(_make_frame_lines(samples, sr=16000))
        assert frame is not None
        # 1600 samples @ 16 kHz = 100 ms.
        self.assertAlmostEqual(frame.duration_ms, 100.0)


if __name__ == "__main__":
    unittest.main()
