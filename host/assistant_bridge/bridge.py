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
        choices=("bleak", "file-inject"),
        default="file-inject",
        help="Uplink source. 'bleak' requires a connected PSE84 kit.",
    )
    p.add_argument("--input", type=Path, help="WAV path for --transport=file-inject")
    p.add_argument(
        "--ollama-url",
        default=os.environ.get("OLLAMA_URL", "http://localhost:11434"),
        help="Ollama base URL (env: OLLAMA_URL)",
    )
    p.add_argument("--ollama-model", default="glm-4.7")
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
        llm = OllamaLLM(base_url=args.ollama_url, model=args.ollama_model)
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


async def _amain(argv: Optional[list[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.transport == "file-inject":
        return await _run_file_inject(args)
    return await _run_bleak(args)


def main(argv: Optional[list[str]] = None) -> int:
    return asyncio.run(_amain(argv))


if __name__ == "__main__":
    sys.exit(main())
