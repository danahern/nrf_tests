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
