# pse84_assistant

PSE84 Voice Assistant PoC (Zephyr). See the master plan at
`.claude/plans/compiled-snuggling-nygaard.md` for the full roadmap.

Phase 1 (Track A) lands the four-state LVGL animation + state machine:
`sw0` (gpio_keys / INPUT_KEY_0, SDL SPACE under native_sim) cycles
`IDLE → LISTENING → THINKING → RESPONDING → IDLE`, each swapping a
procedural animation. See [`docs/ANIMATIONS.md`](docs/ANIMATIONS.md)
for the sprite-sheet-vs-procedural tradeoff.

## Hardware build (PSE84 kit)

```bash
cd zephyr_workspace/zephyrproject && source zephyr/zephyr-env.sh
cd -
west build -b kit_pse84_eval/pse846gps2dbzc4a/m55 \
    -s zephyr_workspace/pse84_assistant -p always --sysbuild
```

Flashing is covered elsewhere (`reference_pse84_flash_addresses.md` — M33
`--hex-addr 0x60100000`, not `0x22011000`).

## Off-hardware development (native_sim)

`native_sim` lets the LVGL + input path be verified visually on a Linux
host (or a Linux container from macOS) with no PSE84 hardware. The SDL
display driver provides a window, and `zephyr,gpio-emul-sdl` maps an SDL
keyboard scancode to the same `gpio_keys` / INPUT_KEY_0 path used on HW,
so `src/main.c` runs unmodified.

Files that drive this:
- `boards/native_sim.overlay` — SDL display chosen node, `sw0` → gpio0
  pin 1 → SDL SCANCODE_SPACE (44).
- `prj_native_sim.conf` — strips PSE84-specific peripherals (GFXSS, I2C,
  octal flash, shell) and turns on the SDL display + `CONFIG_LV_COLOR_DEPTH_32`
  (native_sim SDL runs RGBA8888).

### Build

Zephyr's POSIX arch (which backs `native_sim`) is Linux-only — macOS can
NOT build or run the binary directly. Two options:

**Option A — native Linux host**:
```bash
source zephyr_workspace/zephyrproject/zephyr/zephyr-env.sh
west build -b native_sim/native/64 \
    -d build_native \
    -s zephyr_workspace/pse84_assistant \
    -p always \
    -- -DCONF_FILE=prj_native_sim.conf

./build_native/zephyr/zephyr.exe
# Press SPACE in the SDL window to cycle states:
#   IDLE       -> gradient orb breathing (size + opacity over 2 s)
#   LISTENING  -> 5-bar VU meter, heights animated out of phase
#   THINKING   -> rotating lv_spinner (amber arc)
#   RESPONDING -> typing-effect label with blinking cursor
# The top-of-screen status label always shows the current state name.
```

**Option B — macOS via OrbStack / Docker (build only; SDL window needs
X forwarding or a Linux desktop to actually render)**:
```bash
docker run --rm -it \
    -v "$HOME/code/claude:$HOME/code/claude" \
    -v "$HOME/zephyr-sdk-1.0.0:/opt/toolchains/zephyr-sdk-1.0.0" \
    -w "$PWD" \
    -e ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-1.0.0 \
    ghcr.io/zephyrproject-rtos/ci:v0.28.7 bash -c '
    source zephyr_workspace/zephyrproject/zephyr/zephyr-env.sh &&
    west build -b native_sim/native/64 -d build_native_docker \
        -s zephyr_workspace/pse84_assistant -p always \
        -- -DCONF_FILE=prj_native_sim.conf'
```

Running the binary from the container prints the `pse84_assistant`
banner via the emulated UART but SDL display init fails without a
display server. For visual verification use Option A or add X
forwarding (Linux VM / XQuartz + network X).

## Running tests (twister, native_sim)

The `tests/` tree carries Ztest suites exercised from the Docker-based CI
container so the host toolchain mismatch doesn't matter. Both suites run
on `native_sim/native/64`.

```bash
docker run --rm \
    -v "$HOME/code/claude:$HOME/code/claude" \
    -w "$PWD" -e HOME=/tmp \
    -e ZEPHYR_SDK_INSTALL_DIR=/opt/toolchains/zephyr-sdk-1.0.1 \
    ghcr.io/zephyrproject-rtos/ci:v0.29.1 \
    bash -lc 'source zephyr_workspace/zephyrproject/zephyr/zephyr-env.sh && \
        west twister -p native_sim/native/64 \
            -T zephyr_workspace/pse84_assistant/tests --inline-logs'
```

Suites:

- `tests/framing/` — pure-C `src/framing.c` coverage: encode/decode
  round-trip, streaming parser (partial SDUs, multiple frames per
  feed, mid-frame splits, unknown-type resync), bad-args handling.
  Mirrors `host/assistant_bridge/tests/test_framing.py` so the wire
  format is verified from both ends. **No libopus submodule required.**
- `tests/opus_roundtrip/` — instantiates `src/opus_wrapper.c` with
  CONFIG_OPUS=y, feeds a 1 kHz sine @ 16 kHz through encode → decode,
  asserts RMS energy is preserved (0.5x – 2x) after a 600 ms warm-up
  window. Requires the `zephyr_workspace/modules/libopus/opus`
  submodule to be initialised (`git submodule update --init --recursive`).

### Opus module (Track C)

libopus (xiph/opus, pinned to v1.5.2) is vendored under
`zephyr_workspace/modules/libopus/` as a Zephyr out-of-tree module. The
app `CMakeLists.txt` registers it via `ZEPHYR_EXTRA_MODULES` so no
patching of `zephyrproject/zephyr/west.yml` is required. Build options
force `FIXED_POINT=1` + `DISABLE_FLOAT_API` — no FPU dependency, and
both encoder and decoder are always present so the v3 TTS path can
reuse the same build.

- **CONFIG_OPUS=n (default)**: the `opus` INTERFACE target exists but
  contributes no sources; `opus_wrap_*` entry points return `-ENOTSUP`.
  The pse84_assistant HW + native_sim builds do not require the
  submodule to be checked out.
- **CONFIG_OPUS=y**: pulls the vendored sources and compiles in ~60
  Opus TUs. Roughly 450 KB flash on M55.

Initialise the submodule if you need the real codec:

```bash
git submodule update --init --recursive zephyr_workspace/modules/libopus/opus
```

### Behavior gaps vs. HW (for Tracks A and C)

- **Display resolution**: native_sim's SDL display defaults to 320×240
  (see `zephyr/drivers/display/display_sdl.c`); HW GFXSS is 800×480.
  UI layouts anchored with `LV_ALIGN_*` ports fine; fixed-coordinate
  placement (e.g. sprite sheets) will look offset.
- **Color depth**: native_sim is RGBA8888 (`LV_COLOR_DEPTH_32`); HW is
  RGB565 (`LV_COLOR_DEPTH_16`). If any sprite/asset bakes a depth, it
  will need both variants.
- **Input**: SDL keyboard (SPACE) maps to INPUT_KEY_0 — same input_cb in
  `src/main.c`. No scan-rate difference material for Track A animations.
- **No DSI panel bridge**: `display_blanking_off` is a no-op on the SDL
  display driver (returns `-ENOSYS`, which main.c already tolerates).
- **No octal flash / PSRAM / I2C**: any track that starts depending on
  XIP assets or the panel MCU (Track A sprite sheets from the 10 MB
  XIP region, Track C Opus from PSRAM if that lands) will need to
  either link assets into RAM for native_sim, or gate the dependency
  with `#ifdef CONFIG_BOARD_NATIVE_SIM`.
- **No M33 companion / IPC**: sysbuild is NOT used in the native_sim
  path. Any Phase 0b IPC work must provide a native_sim stub (e.g., a
  Zephyr-side mock endpoint) before it can be exercised here.
