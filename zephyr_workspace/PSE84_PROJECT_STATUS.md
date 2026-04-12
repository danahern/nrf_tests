# PSE84 Zephyr Project — State of the World

Top-level index for all PSE84-related work in this repository.
Last updated: 2026-04-11.

## TL;DR

- **Display driver series for the PSOC Edge E84 GFXSS is complete and pushed to a fork of zephyrproject-rtos/zephyr.** 17 clean commits, all DCO-signed, all checkpatch-clean, all verified on KIT_PSE84_EVAL hardware. Ready for upstream PR when you want to open it.
- **Two runnable animation demos** — fuzzy character (`pse84_video_test`) and 1990s cartoon duck (`pse84_cartoon_test`) — play 125-frame 24 fps loops on the Waveshare 4.3" DSI panel via the driver.
- **LVGL Hello World** runs on the same driver via the upstream `samples/subsys/display/lvgl` sample.
- **Octal flash (128 MB Infineon SEMPER S28HS01GT) enablement is mid-flight.** Research complete, Infineon-generated config artifacts captured, integration into Zephyr not yet attempted.

---

## Directory map

```
zephyr_workspace/
├── PSE84_PROJECT_STATUS.md               <- you are here
├── PSE84_OCTAL_FLASH_ENABLEMENT_PLAN.md  <- original octal design doc
├── PSE84_OCTAL_RESEARCH_FINDINGS.md      <- what the web/tool research unlocked
├── pse84_i2c_test/                       <- minimal display test (4 color bars)
├── pse84_video_test/                     <- fuzzy character animation demo
├── pse84_cartoon_test/                   <- 1990s cartoon duck animation demo
├── pse84_octal_enablement/               <- octal flash local workspace
│   ├── README.md
│   ├── design_src/design.cyqspi
│   └── cycfg_octal_generated/
│       ├── cycfg_qspi_memslot.c
│       ├── cycfg_qspi_memslot.h
│       ├── qspi_config.cfg
│       └── PSE84_SMIF.FLM  (LFS)
└── zephyrproject/                        <- west workspace (not checked in)
    └── zephyr/                           <- the actual zephyr source tree
                                             where the 17-commit branch lives
```

---

## 1. Display driver series (done, pushed)

**Remote branch:** `danahern/zephyr@pse84-gfxss-display-driver`
**Base:** upstream `zephyrproject-rtos/zephyr/main`
**Commits:** 17, all DCO-signed to `danahern@gmail.com`, all pass `scripts/checkpatch.pl`
**Compare URL:** https://github.com/danahern/zephyr/compare/main...pse84-gfxss-display-driver

### Commit list (bottom-up order)

| # | SHA prefix | Title | Scope |
|---|---|---|---|
| 1 | `0ff99ffb03b` | drivers: i2c: infineon_pdl: fix SCB timing for PSE84 100 kHz master | Compute SCB OVS from DT peri-clock — unblocks the panel MCU over I2C |
| 2 | `155eea34f40` | soc: infineon: pse84: fix cy_ppc_unsecure_init enum-gap early exit | Don't bail the whole loop on a single enum-gap invalid region |
| 3 | `4bb71d13e68` | soc: infineon: pse84: enable GFXSS peripheral groups for CM55 | Peri-group init for GPU / DC / MIPI-DSI so the M55 can access the IP |
| 4 | `1e0ead989e2` | modules: hal_infineon: plumb GFXSS HAL sources into the build | Kconfig + CMakeLists wiring for cy_graphics.c, cy_mipidsi.c, viv_dc_*.c |
| 5 | `20391201666` | drivers: clock_control: infineon: add pinctrl support for fixed-clock | Lets `clk_ext` own pinctrl-0 so the 24 MHz EXT clock wires up from DT |
| 6 | `b92aa26da28` | drivers: display: add Waveshare 4.3" MCU DSI panel driver | I2C panel driver using `zephyr,deferred-init` for post-DSI-up init |
| 7 | `ad4c2d2f24a` | drivers: display: add Infineon PSE84 GFXSS display controller | The main driver — video-mode DSI, SOCMEM framebuffer |
| 8 | `18557c45ff3` | drivers: display: pse84_gfxss: clean D-cache only over dirty range | Per-write cache clean optimization |
| 9 | `d647e3a6155` | drivers: display: pse84_gfxss: split framebuffer stride from panel width | New `stride-pixels` binding property |
| 10 | `64cb48b937e` | drivers: display: pse84_gfxss: take framebuffer as a phandle | FB partition via phandle, not raw reg |
| 11 | `1978510ed6a` | MAINTAINERS: route pse84 drivers to Infineon Platforms | Glob `drivers/*/*pse84*` added |
| 12 | `533eda5e550` | drivers: display: pse84_gfxss: drive D-PHY reference clock from DT | Removes in-driver SRSS pokes, uses standard `clk_hf12` DT chain |
| 13 | `0344d773ccf` | tests: drivers: build_all: display: cover pse84-gfxss and waveshare 4p3 mcu | CI build coverage for both new drivers |
| 14 | `e2f5dc17bf8` | samples: drivers: display: add kit_pse84_eval overlay for GFXSS | Lets `samples/drivers/display` run on the kit |
| 15 | `59334212488` | samples: subsys: display: lvgl: add kit_pse84_eval overlay | Lets `samples/subsys/display/lvgl` run on the kit |
| 16 | `2588bcf2fa6` | soc: infineon: pse84: extend M55 SMIF0 MPC region to 11 MB | Unlocks M55 apps larger than 3 MB (required for video demos) |
| 17 | `bbc0f60fcc9` | drivers: flash: infineon_serial_memory_qspi: select chip-select from DT | New `chip-select` / `io-mode` / `data-rate` / `rx-capture-mode` / `smif-frequency` properties on `infineon,qspi-flash` — infrastructure for future octal support |

### How to submit upstream when ready

Not done yet. The branch is PR-ready. When you want to push the button:

```bash
cd zephyr_workspace/zephyrproject/zephyr
gh pr create --repo zephyrproject-rtos/zephyr \
    --base main --head danahern:pse84-gfxss-display-driver \
    --title "drivers: display: add Infineon PSE84 GFXSS + Waveshare 4.3\" panel" \
    --body-file ../../PR_BODY.md
```

I'll draft the PR_BODY when you give the go-ahead.

---

## 2. Animation demos (done, running on hardware)

Both use the same pipeline:

1. `ffmpeg` extracts source frames at a reduced resolution into a raw RGB565 blob
2. Zephyr build embeds the blob via `generate_inc_file_for_target` as a `const` in flash (m55_xip partition)
3. M55 app loops the frames via `display_write()` at native 24 fps, with integer pixel upscale in the driver to fill the panel
4. `k_uptime_get()`-paced frame scheduling for stable 24 fps

### pse84_video_test

- **Source:** `~/Downloads/a-looping-3-second-animation-of-a-big-round-fuzzy- (1).mp4` (832×464 landscape, 24 fps, 125 frames, 5.208 s)
- **Baked resolution:** 240×144 (via `ffmpeg -vf "scale=258:144:flags=lanczos,crop=240:144"`)
- **Upscale:** 3× → 720×432 centered on 800×480 panel, 40/24 px black letterbox
- **Blob size:** 8.64 MB (125 × 240 × 144 × 2)
- **App binary total:** ~8.74 MB (fits in the 10 MB `m55_xip` partition after the MPC bump)

### pse84_cartoon_test

- **Source:** `~/Downloads/animate-1990s-saturday-morning-cartoon-style-bold-.mp4` (464×832 **portrait**, 24 fps, 125 frames)
- **Pre-rotation:** `ffmpeg -vf "transpose=1,scale=258:144:flags=lanczos,crop=240:144"` — clockwise 90° before resize, so the landscape framebuffer appears right-side-up when the panel is physically mounted in portrait
- **Everything else identical to `pse84_video_test`** — same 240×144 → 3× → 720×432 layout, same `main.c`

### Shared infrastructure

Both apps depend on a **local-only** `m55_xip` partition override (2 MB → 10 MB) in their board overlays. This is not upstream — it's a per-app DT tweak. The 10 MB extension eats into the 9 MB of free flash past the stock m55_xip boundary, which would be reclaimed by the octal flash migration.

The **SMIF0 MPC region bump** (3 MB → 11 MB, commit `2588bcf2fa6` on the upstream branch) IS upstream-worthy because the 3 MB cap was an arbitrary limit that blocks any M55 app above 3 MB regardless of what the app is doing.

---

## 3. LVGL integration (done, running on hardware)

The canonical `samples/subsys/display/lvgl` sample runs on KIT_PSE84_EVAL via a board overlay I added at `samples/subsys/display/lvgl/boards/kit_pse84_eval_pse846gps2dbzc4a_m55.overlay` (upstream branch commit `59334212488`). Board conf bumps `CONFIG_LV_Z_MEM_POOL_SIZE` to 64 KB (the default 16 KB is too small for 800×480) and `CONFIG_MAIN_STACK_SIZE` to 8 KB.

Result: standard LVGL Hello World + click counter button rendering on the panel at ~167 KB BSS usage, proving the display driver integrates cleanly with Zephyr's LVGL subsystem. LVGL's partial-redraw virtual display buffer (VDB) model works correctly with the driver's row-by-row `display_write` path.

---

## 4. Octal flash enablement (research done, integration pending)

### The prize

Replacing the 16 MB S25FS128S Quad NOR on SMIF0 CS1 with the **128 MB Infineon SEMPER S28HS01GT Octal NOR on SMIF0 CS0** would:
- **8× the storage** (128 MB vs 16 MB)
- **Remove the "downscale frames" constraint** — native 800×480 RGB565 animation at 24 fps becomes trivially representable (93.8 MB for a 5.2 s loop)
- **Enable LVGL asset libraries** at meaningful scale (fonts, images, video textures)
- **Unlock over-the-wire OTA** with dual MCUboot slots
- **Roughly 5× the bandwidth** at peak (400 MBps DDR vs 80 MBps quad DDR)

### Why it's hard

1. **Shared IO lines.** Both flashes sit on SMIF0 DQ0..3; only one can be electrically active at a time. You commit the whole boot chain to one flash; no runtime switching.
2. **No upstream Zephyr precedent.** The existing `infineon,qspi-flash` driver and `cycfg_qspi_memslot.c` are hardcoded for the 16 MB S25FS128S on CS1. The `kit_pse84_ai` sibling board uses QSPI + HyperRAM only — no octal NOR.
3. **MTB SmartwatchDemo doesn't use octal either.** There is no known-good working reference inside the Zephyr/MTB ecosystem to copy.
4. **Live SMIF reconfiguration.** If we're XIP-ing from quad at the moment we switch to octal, the reconfigure sequence itself bus-faults the running code. Needs SRAM-resident transition code.

### What I've unblocked

See `PSE84_OCTAL_RESEARCH_FINDINGS.md` for details. Short version:

1. **The Extended Boot ROM does NOT pin the SMIF mode.** AN237857 §6 documents the recipe: the M33 secure image (EPB or equivalent) reconfigures SMIF itself during its own init, from SRAM-resident code. No fuse writes, no SE asset-table updates, no provisioning-level access needed.

2. **Infineon ships a pre-canned OPI DDR preset for this exact chip** in the ModusToolbox QSPI Configurator memory database: `S28HS01GT (Octal DDR Hybrid at Bottom 25 MHz)`. Every "risky hand-writing" value from the original plan (dummy cycles, command pairs, octal-enable register addresses) is in that preset.

3. **I successfully ran the QSPI Configurator CLI** (`/Applications/ModusToolbox/tools_3.7/qspi-configurator/qspi-configurator-cli`) against a modified `design.cyqspi` and got real output:
   - `cycfg_qspi_memslot.c/h` — 578-line Infineon-generated octal memslot config (saved to `pse84_octal_enablement/cycfg_octal_generated/`)
   - `qspi_config.cfg` — openocd bank def at `0x60000000` size `0x4000000` (64 MB — first XIP aperture)
   - `PSE84_SMIF.FLM` — the **patched** 488 KB CMSIS flashloader with the new memslot baked in (LFS)

4. **The 128 MB chip maps as two 64 MB slots.** SMIF0 per-aperture limit is 64 MB. To use all 128 MB you declare two slots, one at `0x60000000..0x63FFFFFF` and one at `0x64000000..0x67FFFFFF`. For animation-sized assets, 64 MB in the first aperture is plenty.

### What's still open

1. **Integrate `cycfg_qspi_memslot.c` into the Zephyr build** — likely by dropping an alternate file into `modules/hal_infineon/zephyr-ifx-cycfg/kit_pse84_eval/` and gating selection on a new `CONFIG_INFINEON_SMIF_OCTAL` Kconfig.

2. **Write the SRAM-resident SMIF transition code.** This is the real engineering risk: the Cypress PDL's `Cy_SMIF_Init()` + `Cy_SMIF_MemInit()` must run from an address that doesn't depend on SMIF (i.e. SRAM/ITCM), and it needs to be called before any XIP execution from the new octal aperture. Needs `__attribute__((section(".ramfunc")))`-style linker placement and a careful ordering.

3. **Update board memory map + partition table** per AN237857 Figure 53 (documented in the plan).

4. **Wire a new openocd config variant** that loads the patched FLM and declares the new bank at `0x60000000` size `0x4000000`.

5. **Hardware smoke test.** The failure modes are (a) SMIF transition bus-faults, (b) ROM refuses to boot the re-targeted M33 image, (c) flashloader programs the wrong bytes. All are debuggable with the artifacts in hand.

---

## 5. Demo app repo tracking

All runnable app code + generated assets is tracked in `danahern/nrf_tests` on GitHub:

| Branch | HEAD | Purpose |
|---|---|---|
| `main` | `f15fdb6` | Apps + plan + generated octal artifacts |

LFS-tracked files (for size reasons):
- `zephyr_workspace/pse84_video_test/src/frames.bin` (8.64 MB)
- `zephyr_workspace/pse84_cartoon_test/src/frames.bin` (8.64 MB)
- `zephyr_workspace/pse84_octal_enablement/cycfg_octal_generated/PSE84_SMIF.FLM` (488 KB)

`.gitattributes` pattern: `zephyr_workspace/pse84_*_test/src/*.bin` and `zephyr_workspace/pse84_octal_enablement/cycfg_octal_generated/*.FLM`

---

## 6. Non-blocking latent issues

### M33 companion hardfault

The `enable_cm55` sysbuild M33 companion (which is `samples/basic/minimal` + `CONFIG_SOC_PSE84_M55_ENABLE=y`) reaches Zephyr's fatal-error spin loop at PC `0x1810627a` after successfully calling `Cy_SysEnableCM55()`. M55 has already been started by then, so the display pipeline is unaffected. The HardFault source is somewhere in the tail of `ifx_pse84_cm55_startup()` in `soc/infineon/edge/pse84/security_config/pse84_boot.c` — probably `cy_ppc0_init` / `cy_ppc1_init` / `sys_clock_disable`, but not root-caused.

Status: **deferred, non-blocking**, tracked in memory under `project_pse84_m33_hardfault.md`. Should be filed as a separate PSE84 SoC bug when the display driver goes upstream.

### Halting CM33 via openocd disrupts the display

Empirically, running `openocd ... targets cat1d.cm33; halt; ...` on a booted system sometimes leaves the M55 in a state where subsequent `reset run` from openocd doesn't re-start it cleanly. A full power-cycle recovers. Not a driver issue — a JLink/openocd interaction with the PSE84 dual-core wait-state.

---

## 7. Other workspace contents (not part of this project)

The `zephyr_workspace/` directory also holds several unrelated projects from earlier work — nRF54L15, nRF54LM20, power comparison etc. These are all runnable but independent of the PSE84 display work. See their individual READMEs under `zephyr_workspace/nrf54l*_test/` and `zephyr_workspace/power_comparison/`.

The `SmartwatchDemo_Edit/` directory is a local copy of Infineon's ModusToolbox PSE84 smartwatch demo used as a known-working reference for the display bring-up. It uses the 16 MB Quad flash, not octal — checked during the research pass.
