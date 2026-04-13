"""PSE84 voice-assistant host bridge — entrypoint.

Two transports share the same :class:`assistant_bridge.pipeline.AssistantPipeline`:

  * ``--transport=bleak`` — real BLE CoC (requires a connected PSE84 kit).
  * ``--transport=file-inject --input=<wav>`` — feeds a local 16 kHz mono WAV
    through the same frame path, for wire-format + pipeline development
    without hardware.

The two paths only differ in how :class:`Frame` instances are pushed in and
how outbound frames are sent out: the frame protocol, Opus codec, Whisper
transcriber, and Ollama streaming are shared.
"""
from __future__ import annotations

import argparse
import asyncio
import os
import sys
from pathlib import Path
from typing import Optional

from .framing import Frame, FrameType, StreamingFrameParser, decode_frame
from .pipeline import (
    DEFAULT_SYSTEM_PROMPT,
    AssistantPipeline,
    DryRunLLM,
    OllamaLLM,
    PipelineConfig,
    build_fake_transcriber,
)


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="assistant-bridge")
    p.add_argument(
        "--transport",
        choices=("bleak", "file-inject", "uart", "mac-mic"),
        default="file-inject",
        help=(
            "Uplink source. 'bleak' requires a connected PSE84 kit; "
            "'uart' reads the Phase 2 PCM_BEGIN/hex/PCM_END protocol over "
            "/dev/cu.usbmodem*."
        ),
    )
    p.add_argument("--input", type=Path, help="WAV path for --transport=file-inject")
    p.add_argument(
        "--ollama-url",
        default=os.environ.get(
            "OLLAMA_URL", "http://192.168.1.129:11434"
        ),
        help="Ollama base URL (env: OLLAMA_URL)",
    )
    p.add_argument("--ollama-model", default="glm-4.7-flash:latest")
    p.add_argument(
        "--system-prompt",
        default=DEFAULT_SYSTEM_PROMPT,
        help="System prompt sent to Ollama on every turn",
    )
    p.add_argument(
        "--uart-port",
        default=None,
        help="Serial device (default: auto-glob /dev/cu.usbmodem*)",
    )
    p.add_argument(
        "--uart-baud",
        type=int,
        default=460800,
        help="UART baud (must match pse84_assistant firmware, default 460800)",
    )
    p.add_argument(
        "--uart-port-glob",
        default="/dev/cu.usbmodem*",
        help="Glob used when --uart-port is not set",
    )
    p.add_argument(
        "--dry-run-llm",
        action="store_true",
        help="Skip the Ollama HTTP call; return a canned reply",
    )
    p.add_argument(
        "--fake-transcript",
        default="what is the capital of france",
        help="Fake transcript to use when opus/whisper are unavailable",
    )
    p.add_argument(
        "--real",
        action="store_true",
        help="Use real opuslib + faster-whisper (requires native libs)",
    )
    p.add_argument("--text-chunk-bytes", type=int, default=32)
    p.add_argument("--bleak-address", help="Target device address (bleak transport)")
    # mac-mic transport options
    p.add_argument(
        "--mac-mic-device",
        default=None,
        help="sounddevice input device name/index (mac-mic transport; default: system default)",
    )
    p.add_argument(
        "--mac-mic-max-seconds",
        type=float,
        default=8.0,
        help="Max recording length per SPACE-toggle window (mac-mic transport)",
    )
    p.add_argument(
        "--mac-mic-sample-rate",
        type=int,
        default=16000,
        help="Capture sample rate for mac-mic transport (default 16000 to match device)",
    )
    return p


def _build_codecs(args: argparse.Namespace):
    """Return (opus, transcriber, llm) honoring --real and --dry-run-llm."""
    if args.real:
        from .pipeline import FasterWhisperTranscriber, OpuslibCodec

        opus = OpuslibCodec()
        transcriber = FasterWhisperTranscriber()
    else:
        opus = _IdentityOpus()
        transcriber = build_fake_transcriber(args.fake_transcript)

    if args.dry_run_llm:
        llm = DryRunLLM(
            reply=f"(dry-run) You said: '{args.fake_transcript}'. Canned reply."
        )
    else:
        llm = OllamaLLM(
            base_url=args.ollama_url,
            model=args.ollama_model,
            system_prompt=args.system_prompt,
        )
    return opus, transcriber, llm


class _IdentityOpus:
    """Pass-through codec used when ``--real`` is not set. Keeps the
    file-inject path working without opuslib installed."""

    def encode(self, pcm_bytes: bytes, frame_size: int) -> bytes:
        return pcm_bytes

    def decode(self, opus_bytes: bytes, frame_size: int) -> bytes:
        # When the matching encoder is identity, the input is raw PCM.
        # If a shorter buffer arrives, zero-pad.
        needed = frame_size * 2
        if len(opus_bytes) >= needed:
            return opus_bytes[:needed]
        return opus_bytes + b"\x00" * (needed - len(opus_bytes))


async def _run_file_inject(args: argparse.Namespace) -> int:
    if not args.input:
        print("--transport=file-inject requires --input=<wav>", file=sys.stderr)
        return 2
    if not args.input.exists():
        print(f"input WAV does not exist: {args.input}", file=sys.stderr)
        return 2

    opus, transcriber, llm = _build_codecs(args)
    cfg = PipelineConfig(
        ollama_url=args.ollama_url,
        ollama_model=args.ollama_model,
        text_chunk_bytes=args.text_chunk_bytes,
        dry_run_llm=args.dry_run_llm,
    )

    async def stdout_outbound(raw: bytes):
        frame, _ = decode_frame(raw)
        label = frame.type.name
        if frame.type == FrameType.TEXT_CHUNK:
            body = frame.payload.decode("utf-8", errors="replace")
            print(f"OUT {label} seq={frame.seq} len={len(frame.payload)} {body!r}")
        else:
            print(f"OUT {label} seq={frame.seq} len={len(frame.payload)}")

    pipeline = AssistantPipeline(
        config=cfg,
        opus=opus,
        transcriber=transcriber,
        llm=llm,
        on_outbound=stdout_outbound,
    )

    # Pre-flight: duration estimate.
    import wave

    with wave.open(str(args.input), "rb") as w:
        duration_s = w.getnframes() / float(w.getframerate())
    print(f"file-inject: {args.input} duration={duration_s:.3f}s")

    result = await pipeline.process_wav(args.input)
    print(
        f"file-inject done: {result.audio_frames_encoded} audio frames, "
        f"transcript={result.transcript!r}, "
        f"reply={result.reply_text!r}, "
        f"text_chunks_sent={result.text_chunks_sent}"
    )
    return 0


async def _run_bleak(args: argparse.Namespace) -> int:
    """Real BLE CoC transport. Intentionally minimal — hardware-bring-up
    details land when Phase 4 boards come up. This wires the common
    pipeline to a bleak L2CAP client so the framing + Opus + LLM path is
    exercised end-to-end without code-path drift between transports."""
    try:
        from bleak import BleakClient, BleakScanner  # type: ignore
    except Exception as e:  # pragma: no cover — bleak present in env
        print(f"bleak import failed: {e}", file=sys.stderr)
        return 2

    if not args.bleak_address:
        print("--transport=bleak requires --bleak-address=<addr>", file=sys.stderr)
        return 2

    opus, transcriber, llm = _build_codecs(args)
    cfg = PipelineConfig(
        ollama_url=args.ollama_url,
        ollama_model=args.ollama_model,
        text_chunk_bytes=args.text_chunk_bytes,
        dry_run_llm=args.dry_run_llm,
    )

    outbound_q: asyncio.Queue[bytes] = asyncio.Queue()

    async def on_outbound(raw: bytes):
        await outbound_q.put(raw)

    pipeline = AssistantPipeline(
        config=cfg,
        opus=opus,
        transcriber=transcriber,
        llm=llm,
        on_outbound=on_outbound,
    )
    parser = StreamingFrameParser()

    # NOTE: bleak's public API does not ship a portable L2CAP CoC client on
    # every platform. On macOS CoreBluetooth exposes L2CAP channels via
    # `CBPeripheral.openL2CAPChannel(_:)`, which is surfaced through PyObjC.
    # Finalizing this transport needs hardware to validate the exact byte
    # stream — we scaffold the control flow here so wiring it up is a
    # one-file change once the PSE84 side advertises.
    print(
        "bleak transport not finalized — requires hardware to validate. "
        "See README for the macOS PyObjC openL2CAPChannel path.",
        file=sys.stderr,
    )
    return 3


def _resolve_uart_port(args: argparse.Namespace) -> Optional[str]:
    """Resolve the serial port path for the UART transport.

    Returns ``None`` if no port was supplied and the glob had no hits —
    callers treat that as a fatal configuration error.
    """
    # Lazy import so the bridge still imports cleanly if the audio_capture
    # tree is ever packaged separately.
    from audio_capture.uart_protocol import find_serial_port

    if args.uart_port:
        return args.uart_port
    return find_serial_port(args.uart_port_glob)


def _make_uart_line_source(port: str, baud: int):
    """Return an iterator of stripped ASCII lines from the serial port.

    Factored out so ``_run_uart`` can be unit-tested by swapping in a
    canned iterator. Blocks in the serial read loop; terminate via
    ``KeyboardInterrupt``.
    """
    from audio_capture.uart_protocol import _serial_lines

    return _serial_lines(port, baud)


async def _run_uart(
    args: argparse.Namespace,
    *,
    line_source_factory=None,
) -> int:
    """UART transport: read PCM captures from the PSE84 kit, transcribe,
    stream the LLM reply to stdout.

    ``line_source_factory`` lets tests inject a deterministic iterable
    in place of the real serial read loop.
    """
    from audio_capture.uart_protocol import iter_captures

    port: Optional[str] = None
    if line_source_factory is None:
        port = _resolve_uart_port(args)
        if not port:
            print(
                f"ERROR: no serial device matched glob {args.uart_port_glob!r}",
                file=sys.stderr,
            )
            return 2

    opus, transcriber, llm = _build_codecs(args)
    cfg = PipelineConfig(
        ollama_url=args.ollama_url,
        ollama_model=args.ollama_model,
        text_chunk_bytes=args.text_chunk_bytes,
        dry_run_llm=args.dry_run_llm,
        system_prompt=args.system_prompt,
    )

    async def stdout_outbound(raw: bytes):
        # Outbound frames are not needed for the Mac-side UART path — the
        # LLM reply streams to stdout via on_token. Swallow the frames so
        # process_pcm's TEXT_END/TEXT_CHUNK emissions don't spam stdout.
        return

    pipeline = AssistantPipeline(
        config=cfg,
        opus=opus,
        transcriber=transcriber,
        llm=llm,
        on_outbound=stdout_outbound,
    )

    if line_source_factory is not None:
        lines = line_source_factory()
        source_desc = "(injected)"
    else:
        assert port is not None
        lines = _make_uart_line_source(port, args.uart_baud)
        source_desc = f"{port} @ {args.uart_baud} baud"

    print(
        f"uart transport listening on {source_desc} "
        f"(Ollama: {args.ollama_url} model={args.ollama_model})",
        file=sys.stderr,
    )

    async def on_token(tok: str) -> None:
        # Stream each reply token to stdout as it arrives, no newline.
        sys.stdout.write(tok)
        sys.stdout.flush()

    try:
        for frame in iter_captures(lines):
            print(
                f"\n[uart] captured {frame.samples} samples "
                f"({frame.duration_ms:.0f} ms, sr={frame.sample_rate})",
                file=sys.stderr,
            )
            result = await pipeline.process_pcm(
                frame.pcm, frame.sample_rate, on_token=on_token
            )
            print(
                f"\n[uart] transcript={result.transcript!r} "
                f"reply_len={len(result.reply_text)}",
                file=sys.stderr,
            )
    except KeyboardInterrupt:
        print("\n[uart] stopped", file=sys.stderr)
    return 0


async def _run_mac_mic(
    args: argparse.Namespace,
    *,
    capture_factory=None,
    input_factory=None,
) -> int:
    """Mac mic transport: capture audio from macOS Core Audio (via sounddevice),
    push-to-talk gated by SPACE in the terminal, feed PCM into the pipeline,
    stream the LLM reply to stdout.

    Independent of any PSE84 hardware. Useful as the demo path while
    on-device PDM is being debugged.

    ``capture_factory`` and ``input_factory`` let tests inject deterministic
    PCM blobs and key sources without requiring sounddevice or a real TTY.
    """
    opus, transcriber, llm = _build_codecs(args)
    cfg = PipelineConfig(
        ollama_url=args.ollama_url,
        ollama_model=args.ollama_model,
        text_chunk_bytes=args.text_chunk_bytes,
        dry_run_llm=args.dry_run_llm,
        system_prompt=args.system_prompt,
    )

    async def stdout_outbound(raw: bytes):
        return  # tokens stream via on_token below; outbound frames not needed

    pipeline = AssistantPipeline(
        config=cfg,
        opus=opus,
        transcriber=transcriber,
        llm=llm,
        on_outbound=stdout_outbound,
    )

    async def on_token(tok: str) -> None:
        sys.stdout.write(tok)
        sys.stdout.flush()

    sample_rate = args.mac_mic_sample_rate

    if capture_factory is None:
        try:
            from .mac_mic import SoundDeviceCapture
        except ImportError as e:
            print(
                f"ERROR: --transport=mac-mic requires the 'sounddevice' "
                f"package: pip install sounddevice ({e})",
                file=sys.stderr,
            )
            return 2
        capture = SoundDeviceCapture(
            device=args.mac_mic_device,
            sample_rate=sample_rate,
            max_seconds=args.mac_mic_max_seconds,
        )
    else:
        capture = capture_factory()

    if input_factory is None:
        from .mac_mic import StdinSpaceToggle

        key_source = StdinSpaceToggle()
    else:
        key_source = input_factory()

    print(
        "mac-mic transport active. Press SPACE to start/stop recording. "
        f"Max {args.mac_mic_max_seconds:.1f} s per turn. Ctrl-C to exit.\n"
        f"Ollama: {args.ollama_url} model={args.ollama_model}",
        file=sys.stderr,
    )

    try:
        async for pcm_bytes in capture.iter_utterances(key_source):
            duration_ms = (len(pcm_bytes) // 2) * 1000.0 / sample_rate
            print(
                f"\n[mac-mic] captured {len(pcm_bytes)//2} samples "
                f"({duration_ms:.0f} ms)",
                file=sys.stderr,
            )
            if len(pcm_bytes) < sample_rate * 2 // 10:  # < 100 ms
                print("[mac-mic] too short, ignoring", file=sys.stderr)
                continue
            result = await pipeline.process_pcm(
                pcm_bytes, sample_rate, on_token=on_token
            )
            print(
                f"\n[mac-mic] transcript={result.transcript!r} "
                f"reply_len={len(result.reply_text)}",
                file=sys.stderr,
            )
    except KeyboardInterrupt:
        print("\n[mac-mic] stopped", file=sys.stderr)
    finally:
        await capture.aclose()
        key_source.close()
    return 0


async def _amain(argv: Optional[list[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.transport == "file-inject":
        return await _run_file_inject(args)
    if args.transport == "uart":
        return await _run_uart(args)
    if args.transport == "mac-mic":
        return await _run_mac_mic(args)
    return await _run_bleak(args)


def main(argv: Optional[list[str]] = None) -> int:
    return asyncio.run(_amain(argv))


if __name__ == "__main__":
    sys.exit(main())
