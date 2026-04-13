#!/usr/bin/env bash
#
# Capture headless LVGL snapshots of pse84_assistant's four states.
#
# Flow:
#   1. Sanity-check Docker / OrbStack is running.
#   2. Build the app in the Zephyr CI container with
#      prj_native_sim_snapshot.conf (CONFIG_APP_SNAPSHOT=y).
#   3. Run the binary headlessly (SDL_VIDEODRIVER=dummy — the SDL display
#      driver still initialises and LVGL still renders, it just doesn't
#      try to open a window). Output PPMs land in $SNAP_DIR on the host.
#   4. Convert PPM → PNG with Pillow (preferred) or `sips` (macOS fallback).
#   5. Delete the PPM intermediates.
#
# The resulting PNGs are committed to the repo so the rendered UI is
# reviewable without rebuilding — see snapshots/*.png.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
# The worktree containing this app (…/embedded or …/embedded-assistant).
WORKTREE_ROOT="$(cd -- "$APP_DIR/../.." && pwd)"
# zephyr_workspace/zephyrproject may be a symlink into a sibling worktree
# (git worktrees share the actual Zephyr checkout by symlink). Resolve
# it so the docker mount covers the real path on the host, not the dead
# symlink inside the container.
ZEPHYRPROJECT_REAL="$(cd -- "$APP_DIR/../zephyrproject" && pwd -P)"
# Common ancestor of WORKTREE_ROOT and ZEPHYRPROJECT_REAL — we mount
# this into the container at the same path so the symlink still resolves.
MOUNT_ROOT="$(cd -- "$WORKTREE_ROOT/.." && pwd)"
SNAP_DIR="$APP_DIR/snapshots"
BUILD_DIR_REL="zephyr_workspace/pse84_assistant/build_native_snapshot"

CI_IMAGE="${CI_IMAGE:-ghcr.io/zephyrproject-rtos/ci:v0.28.7}"
SDK_HOST_DIR="${SDK_HOST_DIR:-$HOME/zephyr-sdk-1.0.0}"
SDK_CONTAINER_DIR="/opt/toolchains/zephyr-sdk-1.0.0"

say() { printf '\033[1;36m[snapshots]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[snapshots]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 1. preflight ----------------------------------------------------
command -v docker >/dev/null 2>&1 || die "docker (or OrbStack) is not on PATH"
if ! docker info >/dev/null 2>&1; then
    die "docker info failed — is OrbStack / Docker Desktop running?"
fi
[[ -d "$SDK_HOST_DIR" ]] || die "Zephyr SDK not found at $SDK_HOST_DIR (override with SDK_HOST_DIR=...)"
[[ -d "$ZEPHYRPROJECT_REAL" ]] || die "zephyrproject not found (resolved to $ZEPHYRPROJECT_REAL)"

mkdir -p "$SNAP_DIR"

# ---- 2. build inside CI container ------------------------------------
say "building with prj_native_sim_snapshot.conf in $CI_IMAGE"
docker run --rm \
    -v "$MOUNT_ROOT:$MOUNT_ROOT" \
    -v "$SDK_HOST_DIR:$SDK_CONTAINER_DIR" \
    -w "$WORKTREE_ROOT" -e HOME=/tmp \
    -e ZEPHYR_SDK_INSTALL_DIR="$SDK_CONTAINER_DIR" \
    "$CI_IMAGE" \
    bash -lc "source zephyr_workspace/zephyrproject/zephyr/zephyr-env.sh && \
        west build -b native_sim/native/64 -d $BUILD_DIR_REL \
            -s zephyr_workspace/pse84_assistant -p always \
            -- -DCONF_FILE=prj_native_sim_snapshot.conf \
               -DEXTRA_DTC_OVERLAY_FILE=boards/native_sim_snapshot.overlay"

# ---- 3. run binary headlessly ----------------------------------------
#
# The embedded-side snapshot code writes PPMs to /tmp/snapshots inside
# the container; bind-mount that path to $SNAP_DIR on the host so the
# files land where we expect. The PPM writer uses O_WRONLY | O_TRUNC
# (no O_CREAT, because the nsi_host_open trampoline doesn't expose
# mode), so we pre-touch empty files here.
say "pre-creating PPM destination files in $SNAP_DIR"
rm -f "$SNAP_DIR"/pse84_assistant_*.ppm
for name in 01_idle 02_listening 03_thinking 04_responding; do
    : > "$SNAP_DIR/pse84_assistant_${name}.ppm"
done

say "running snapshot binary headlessly (SDL_VIDEODRIVER=dummy)"
docker run --rm \
    -v "$MOUNT_ROOT:$MOUNT_ROOT" \
    -v "$SNAP_DIR:/tmp/snapshots" \
    -w "$WORKTREE_ROOT" -e HOME=/tmp \
    -e SDL_VIDEODRIVER=dummy \
    "$CI_IMAGE" \
    "$WORKTREE_ROOT/$BUILD_DIR_REL/zephyr/zephyr.exe"

# ---- 4. convert PPM → PNG --------------------------------------------
PY_BIN="${PY_BIN:-$HOME/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3}"

convert_with_pillow() {
    "$PY_BIN" - "$SNAP_DIR" <<'PYEOF'
import sys, pathlib
from PIL import Image
snap_dir = pathlib.Path(sys.argv[1])
for ppm in sorted(snap_dir.glob("pse84_assistant_*.ppm")):
    png = ppm.with_suffix(".png")
    with Image.open(ppm) as im:
        im.save(png, "PNG", optimize=True)
    print(f"  {ppm.name} -> {png.name} ({im.size[0]}x{im.size[1]})")
PYEOF
}

convert_with_sips() {
    for ppm in "$SNAP_DIR"/pse84_assistant_*.ppm; do
        [[ -s "$ppm" ]] || continue
        # sips needs an output *file* path; it rejects .ppm->.png direct,
        # so round-trip via .bmp (still host-side, then clean up).
        out="${ppm%.ppm}.png"
        sips -s format png "$ppm" --out "$out" >/dev/null
        echo "  $(basename "$ppm") -> $(basename "$out")"
    done
}

if [[ -x "$PY_BIN" ]] && "$PY_BIN" -c 'import PIL' 2>/dev/null; then
    say "converting PPM → PNG via Pillow ($PY_BIN)"
    convert_with_pillow
elif command -v sips >/dev/null 2>&1; then
    say "converting PPM → PNG via sips (macOS)"
    convert_with_sips
else
    die "no PPM→PNG converter: install Pillow in $PY_BIN or run on macOS (sips)"
fi

# ---- 5. clean up intermediates ---------------------------------------
rm -f "$SNAP_DIR"/pse84_assistant_*.ppm

say "done. PNGs in $SNAP_DIR"
ls -l "$SNAP_DIR"/pse84_assistant_*.png
