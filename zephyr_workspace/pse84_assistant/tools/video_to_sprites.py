#!/usr/bin/env python3
"""
video_to_sprites.py — offline Phase 1b asset pipeline.

Input : ~/Downloads/{idle,thinking,working,responding}.mp4
Output: assets/<state>/frames.bin.inc  (C byte list, LZ4 blocks concatenated)
        assets/<state>/frames.h        (frame descriptor table + metadata)

Stages:
  1. ffmpeg extract raw rgb565le frames at target W x H and `fps` for
     `duration` seconds (letterbox to preserve aspect).

     rgb565**le**: LVGL v9 uses LV_COLOR_FORMAT_RGB565 in native
     endian, and the PSE84 M55 is little-endian — so we avoid the
     per-pixel byteswap on decompress by storing LE at the source. The
     master plan mentioned "RGB565 big-endian"; empirically that
     belongs to the cartoon_test raw-blit path, not the LVGL image
     widget path Phase 1b uses.
  2. Iterate frames; LZ4 block-compress each (level=9, high ratio).
  3. Emit .bin.inc (comma bytes) and .h (const descriptor table).

Tier logic:
  - Run tier A (800x480) first.
  - If the total compressed size across all 4 states > 40 MB, rerun at
    tier B (400x240) and document the decision in the tier log.

STATE MAPPING (non-obvious, intentional):
    source video    ->  state-dir / enum
    idle.mp4        ->  assets/idle/       ASSIST_IDLE
    thinking.mp4    ->  assets/listening/  ASSIST_LISTENING
    working.mp4     ->  assets/thinking/   ASSIST_THINKING
    responding.mp4  ->  assets/responding/ ASSIST_RESPONDING
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import lz4.block  # type: ignore
except ImportError as e:  # pragma: no cover - user setup
    print("error: missing `lz4` package. Run: pip install lz4", file=sys.stderr)
    raise SystemExit(1) from e

# --- Source -> state mapping (intentional rename, see module docstring) ---
# (source-basename, state-dirname, C identifier prefix)
STATE_MAP = [
    ("idle",       "idle",       "idle"),
    ("thinking",   "listening",  "listening"),
    ("working",    "thinking",   "thinking"),
    ("responding", "responding", "responding"),
]

TIERS = {
    "A": (800, 480),
    "B": (400, 240),
}
DEFAULT_FPS = 24
DEFAULT_DURATION = 3.0  # seconds — loop length
TIER_A_BUDGET_BYTES = 40 * 1024 * 1024


@dataclass
class FrameRec:
    lz4_off: int   # offset into concatenated lz4 payload
    lz4_len: int   # compressed length
    raw_len: int   # uncompressed length (== W*H*2 always)


def run(cmd: list[str]) -> None:
    r = subprocess.run(cmd, check=False, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"$ {' '.join(cmd)}\n{r.stdout}\n{r.stderr}\n")
        raise SystemExit(f"command failed: {cmd[0]}")


def extract_frames(src_mp4: Path, w: int, h: int, fps: int,
                   duration: float, rotate: str = "cw") -> bytes:
    """ffmpeg -> raw rgb565le frames, letterboxed to WxH, fps fixed, duration capped.

    rotate: "cw" (90 degrees clockwise, default — for kits held in portrait
    tilted-right), "ccw" (90 degrees counterclockwise), or "none" (no
    rotation, native-landscape content). The PSE84 display panel is
    physically landscape (800x480); pre-rotating here lets the kit be
    held in portrait with the character appearing upright without needing
    GFXSS rotation or LVGL software-rotate (which would cost a full
    framebuffer copy every sprite frame).
    """
    # scale to fit + pad to exact WxH (letterbox), force pix_fmt rgb565le.
    filters = []
    if rotate == "cw":
        filters.append("transpose=1")
    elif rotate == "ccw":
        filters.append("transpose=2")
    elif rotate != "none":
        raise SystemExit(f"unknown rotate value: {rotate!r}")
    filters.extend([
        f"scale=w={w}:h={h}:force_original_aspect_ratio=decrease",
        f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=black",
        f"fps={fps}",
    ])
    vf = ",".join(filters)
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-i", str(src_mp4),
        "-t", str(duration),
        "-vf", vf,
        "-f", "rawvideo",
        "-pix_fmt", "rgb565le",
        "-",
    ]
    r = subprocess.run(cmd, check=False, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise SystemExit(f"ffmpeg failed for {src_mp4}")
    return r.stdout


def compress_frames(raw: bytes, frame_bytes: int) -> tuple[bytes, list[FrameRec]]:
    n = len(raw) // frame_bytes
    if n == 0:
        raise SystemExit("no frames extracted (check source / duration)")
    records: list[FrameRec] = []
    blobs: list[bytes] = []
    off = 0
    for i in range(n):
        chunk = raw[i * frame_bytes:(i + 1) * frame_bytes]
        # mode='high_compression' -> HC level (LZ4_compress_HC).
        # Decompressor (lz4-block format) is identical; HC gives ~15-25% better ratio.
        comp = lz4.block.compress(chunk, mode="high_compression",
                                  compression=9, store_size=False)
        records.append(FrameRec(lz4_off=off, lz4_len=len(comp),
                                raw_len=frame_bytes))
        blobs.append(comp)
        off += len(comp)
    return b"".join(blobs), records


def emit_bin_inc(dst: Path, blob: bytes) -> None:
    # One byte per token; 16 per line; hex for readability / compactness.
    lines = []
    for i in range(0, len(blob), 16):
        row = blob[i:i + 16]
        lines.append(", ".join(f"0x{b:02x}" for b in row))
    with dst.open("w") as f:
        f.write(",\n".join(lines))
        f.write("\n")


def emit_header(dst: Path, state_dir: str, prefix: str,
                w: int, h: int, fps: int,
                records: list[FrameRec]) -> None:
    guard = f"ASSETS_{state_dir.upper()}_FRAMES_H_"
    # Note: struct sprite_frame_rec is defined once in src/sprites.h; this
    # header only forward-declares to let the generated frames.c reference
    # it by tag. Consumers include sprites.h for the full definition.
    lines = [
        "/* AUTO-GENERATED by tools/video_to_sprites.py — do not edit. */",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        "#include \"sprite_types.h\"",
        "",
        f"#define {prefix.upper()}_WIDTH   {w}",
        f"#define {prefix.upper()}_HEIGHT  {h}",
        f"#define {prefix.upper()}_FPS     {fps}",
        f"#define {prefix.upper()}_FRAMES  {len(records)}",
        "",
        f"/* Concatenated LZ4 block-format payload, {len(records)} frames. */",
        f"extern const uint8_t {prefix}_lz4_blob[];",
        "",
        f"extern const struct sprite_frame_rec {prefix}_frames[{len(records)}];",
        "",
        f"#endif /* {guard} */",
        "",
    ]
    dst.write_text("\n".join(lines))


def emit_source(dst: Path, prefix: str, records: list[FrameRec],
                blob_size: int, bin_inc_rel: str) -> None:
    # Compile-unit holding the blob (via #include) + frame table.
    lines = [
        "/* AUTO-GENERATED by tools/video_to_sprites.py — do not edit. */",
        f'#include "frames.h"',
        f'#include "sprite_types.h"',
        "",
        f"const uint8_t {prefix}_lz4_blob[] = {{",
        f"#include \"{bin_inc_rel}\"",
        "};",
        "",
        f"const struct sprite_frame_rec {prefix}_frames[{len(records)}] = {{",
    ]
    for r in records:
        lines.append(f"    {{ {r.lz4_off}u, {r.lz4_len}u, {r.raw_len}u }},")
    lines.append("};")
    lines.append("")
    dst.write_text("\n".join(lines))


def process_tier(tier: str, src_root: Path, out_root: Path,
                 fps: int, duration: float, rotate: str = "cw") -> int:
    w, h = TIERS[tier]
    frame_bytes = w * h * 2
    total = 0
    per_state: list[tuple[str, int, int]] = []  # (state, frames, comp_bytes)
    for src_base, state_dir, prefix in STATE_MAP:
        src = src_root / f"{src_base}.mp4"
        if not src.exists():
            raise SystemExit(f"missing source {src}")
        dst_dir = out_root / state_dir
        dst_dir.mkdir(parents=True, exist_ok=True)

        raw = extract_frames(src, w, h, fps, duration, rotate=rotate)
        blob, recs = compress_frames(raw, frame_bytes)

        emit_bin_inc(dst_dir / "frames.bin.inc", blob)
        emit_header(dst_dir / "frames.h", state_dir, prefix, w, h, fps, recs)
        emit_source(dst_dir / "frames.c", prefix, recs, len(blob),
                    "frames.bin.inc")

        total += len(blob)
        per_state.append((state_dir, len(recs), len(blob)))
        print(f"  {state_dir:<10s} {w}x{h}  {len(recs):3d} frames  "
              f"raw={len(raw)/1024:8.1f} KiB  "
              f"comp={len(blob)/1024:8.1f} KiB  "
              f"ratio={len(raw)/max(1,len(blob)):.2f}x")
    return total


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--src", default=str(Path.home() / "Downloads"),
                   help="Directory containing {idle,thinking,working,responding}.mp4")
    p.add_argument("--out", default=None,
                   help="Output root (default: <script>/../assets)")
    p.add_argument("--fps", type=int, default=DEFAULT_FPS)
    p.add_argument("--duration", type=float, default=DEFAULT_DURATION)
    p.add_argument("--force-tier", choices=["A", "B"], default=None,
                   help="Skip auto budget check; use this tier only.")
    p.add_argument("--rotate", choices=["cw", "ccw", "none"], default="cw",
                   help="Pre-rotate frames so the kit can be held in portrait "
                        "(default: cw, 90deg clockwise — compensates for the "
                        "landscape 800x480 panel being tilted 90deg right).")
    args = p.parse_args()

    if not shutil.which("ffmpeg"):
        sys.stderr.write("ffmpeg not on PATH\n")
        return 1

    src_root = Path(args.src).expanduser()
    script_dir = Path(__file__).resolve().parent
    out_root = Path(args.out).expanduser() if args.out else script_dir.parent / "assets"
    out_root.mkdir(parents=True, exist_ok=True)

    tier_log = out_root / "TIER_LOG.md"

    if args.force_tier:
        chosen = args.force_tier
        print(f"[forced] tier {chosen}  ({TIERS[chosen][0]}x{TIERS[chosen][1]})")
        total = process_tier(chosen, src_root, out_root, args.fps, args.duration)
        print(f"total compressed: {total/1024/1024:.2f} MiB")
    else:
        print("[tier A] 800x480")
        total_a = process_tier("A", src_root, out_root, args.fps, args.duration)
        print(f"tier A total compressed: {total_a/1024/1024:.2f} MiB "
              f"(budget {TIER_A_BUDGET_BYTES/1024/1024:.0f} MiB)")
        if total_a <= TIER_A_BUDGET_BYTES:
            chosen = "A"
            total = total_a
        else:
            print("[tier A] exceeded budget — falling back to tier B (400x240)")
            total = process_tier("B", src_root, out_root, args.fps, args.duration)
            chosen = "B"
            print(f"tier B total compressed: {total/1024/1024:.2f} MiB")

    w, h = TIERS[chosen]
    with tier_log.open("w") as f:
        f.write(f"# Phase 1b tier decision\n\n")
        f.write(f"- Chosen tier: **{chosen}** ({w}x{h})\n")
        f.write(f"- fps: {args.fps}\n")
        f.write(f"- duration: {args.duration} s\n")
        f.write(f"- total compressed: {total/1024/1024:.2f} MiB\n")
        f.write(f"- tier A budget: {TIER_A_BUDGET_BYTES/1024/1024:.0f} MiB\n")
        f.write(f"\nIf tier=B, device renders with lv_img_set_zoom(512) (2x).\n")
    print(f"wrote {tier_log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
