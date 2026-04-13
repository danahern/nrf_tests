"""Tests for the extracted uart_protocol module.

Covers :func:`iter_captures` (generator over an arbitrary line iterable)
and the ``find_serial_port`` glob helper — the pieces that
``receive_pcm``'s legacy tests don't reach directly.
"""
from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import uart_protocol  # noqa: E402


def _make_frame_lines(samples: list[int], sr: int = 16000) -> list[str]:
    payload = b"".join(struct.pack("<h", s) for s in samples)
    hex_payload = payload.hex()
    hex_lines = [hex_payload[i : i + 128] for i in range(0, len(hex_payload), 128)]
    return [
        f"=== PCM_BEGIN samples={len(samples)} sample_rate={sr} channels=1 bits=16 ===",
        *hex_lines,
        "=== PCM_END ===",
    ]


class TestIterCaptures(unittest.TestCase):
    def test_yields_multiple_frames(self):
        a = _make_frame_lines([1, 2, 3])
        b = _make_frame_lines([4, 5, 6, 7])
        stream = ["boot log", *a, "intermediate log", *b, "tail"]
        frames = list(uart_protocol.iter_captures(stream))
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[0].samples, 3)
        self.assertEqual(frames[1].samples, 4)

    def test_empty_when_no_begin(self):
        self.assertEqual(list(uart_protocol.iter_captures(["foo", "bar"])), [])

    def test_incomplete_frame_yields_nothing(self):
        # No PCM_END ever arrives — generator exhausts its input silently.
        # (Raising would abort the UART listener for benign truncations.)
        lines = _make_frame_lines([1, 2, 3])[:-1]
        self.assertEqual(list(uart_protocol.iter_captures(lines)), [])

    def test_malformed_hex_length_raises(self):
        lines = _make_frame_lines([1, 2, 3])
        lines[1] = lines[1][:-2]  # corrupt hex payload length
        with self.assertRaises(ValueError):
            list(uart_protocol.iter_captures(lines))

    def test_resyncs_across_begin_restart(self):
        """A new BEGIN without a preceding END should discard the partial
        buffer and start fresh."""
        first = _make_frame_lines([10, 20, 30])
        second = _make_frame_lines([40, 50])
        # Drop the END of the first frame so the second BEGIN restarts the buffer.
        stream = [*first[:-1], *second]
        frames = list(uart_protocol.iter_captures(stream))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].samples, 2)


class TestFindSerialPort(unittest.TestCase):
    def test_returns_first_match_sorted(self):
        with mock.patch(
            "uart_protocol.glob.glob",
            return_value=["/dev/cu.usbmodemB", "/dev/cu.usbmodemA"],
        ):
            self.assertEqual(
                uart_protocol.find_serial_port("/dev/cu.usbmodem*"),
                "/dev/cu.usbmodemA",
            )

    def test_returns_none_when_no_match(self):
        with mock.patch("uart_protocol.glob.glob", return_value=[]):
            self.assertIsNone(uart_protocol.find_serial_port("/no/such/*"))


class TestPcmFrame(unittest.TestCase):
    def test_duration_and_peak(self):
        samples = [0, 1000, -2000]
        frame = uart_protocol.parse_frame(_make_frame_lines(samples, sr=8000))
        assert frame is not None
        self.assertEqual(frame.peak(), 2000)
        # 3 samples @ 8 kHz = 0.375 ms
        self.assertAlmostEqual(frame.duration_ms, 0.375)


class TestIterCapturesFromSerial(unittest.TestCase):
    def test_opens_serial_and_iterates(self):
        """Feed a fake serial object and assert one frame rolls out."""
        lines = [
            line.encode("ascii") + b"\r\n"
            for line in _make_frame_lines([7, 8, 9])
        ]
        # Add a trailing empty readline to break the loop after yielding.
        lines.append(b"")

        class _FakeSerial:
            def __init__(self, port, baudrate, timeout):
                self.port = port
                self.baudrate = baudrate
                self._iter = iter(lines)

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def readline(self):
                try:
                    return next(self._iter)
                except StopIteration:
                    raise KeyboardInterrupt  # break the infinite loop

        fake_serial_module = mock.MagicMock()
        fake_serial_module.Serial = _FakeSerial
        with mock.patch.dict(sys.modules, {"serial": fake_serial_module}):
            gen = uart_protocol.iter_captures_from_serial("/dev/fake", 460800)
            with self.assertRaises(KeyboardInterrupt):
                frames = []
                for f in gen:
                    frames.append(f)
            self.assertEqual(len(frames), 1)
            self.assertEqual(frames[0].samples, 3)


if __name__ == "__main__":
    unittest.main()
