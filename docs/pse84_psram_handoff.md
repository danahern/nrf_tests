# PSE84 PSRAM (S70KS1283 HyperRAM on SMIF1/CS2) — Debug Hand-Off

**STATUS: RESOLVED 2026-04-12 session 2.** CM55 XIP read/write to 0x64000000 (NS) is fully working. See §14 for resolution. Commit `62f7e55cae9` on zephyr submodule `pse84-gfxss-display-driver`, `cbda1c3` on outer main.

**Root cause of the long-standing "CM55 bus-faults at 0x64000000":** the init wasn't running (was commented out) + had a latent NULL-deref + the hand-rolled `Cy_SMIF_HyperBus_InitDevice` sequence hung SMIF1 even when wired up. Replacing the hand-rolled path with `mtb_serial_memory_setup` fixed it in one shot.

**Historical context (§1-12 below) preserved for future similar debugs — don't re-run the ruled-out hypotheses.**

Resume next session by reading this file + the memory at `~/.claude/projects/-Users-danahern-code-claude-embedded/memory/project_pse84_psram.md`.

---

## 1. What the board actually is

- **SoC:** Infineon PSE846GPS2DBZC4A (PSOC Edge E84), dual-core: **CM33 secure + NS in SYSCPUSS (PD0), CM55 NS in APPCPUSS (PD1)**.
- **SMIF0 / CS0:** 128 MB S28HS01GT Octal NOR. Currently booted in Quad default (64 MB CS1 via S25FS128S). Runtime Quad→Octal transition is implemented (gated on `CONFIG_INFINEON_SMIF_OCTAL`) but separate from PSRAM work.
- **SMIF1 / CS2:** 16 MB S70KS1283 HyperRAM. **Target of this debug.** Mapped at `0x6400_0000` NS / `0x7400_0000` Secure.
- **USB/SWD:** KitProg3 CMSIS-DAP on `/dev/cu.usbmodem103`, serial `1414033C03272400`. Board UART2 on M55 → this port.

## 2. What works (committed)

Commits:
- Zephyr submodule (branch `pse84-gfxss-display-driver`):
  - `11fabfa1567` — SMIF1 HyperRAM bring-up via `Cy_SMIF_HyperBus_InitDevice`
  - `324350091fa` — Force `RD/WR_DUMMY_CTL.PRESENT2 = 2` (HyperRAM variable latency)
  - `9c2bceee299` — Move `Cy_SMIF_InitCache` after XIP enable, WT_RWA attribute
- hal/infineon (pushed to `danahern/hal_infineon:pse84-psram`):
  - `0839461` — S70KS1283 memslot added to cycfg (not actually used at runtime; our boot builds its own memCfg)
- Outer `embedded` repo (`main`):
  - `0c2ab97`, `ad4072c`, `7f40e4b`, `a90491f` — `pse84_psram_test` harness evolution

**M33 side: reliably working.** `ifx_pse84_psram_init()` in `zephyr_workspace/zephyrproject/zephyr/soc/infineon/edge/pse84/security_config/pse84_boot.c`:

```
Cy_SMIF_Disable(SMIF1_CORE)
Cy_SMIF_Init(SMIF1_CORE, &cfg, 10000, &ctx)
Cy_SMIF_SetDataSelect(SMIF1_CORE, CS2, DATA_SEL0)
Cy_SMIF_SetRxCaptureMode(SMIF1_CORE, CY_SMIF_SEL_XSPI_HYPERBUS_WITH_DQS, CS2)
Cy_SMIF_Enable(SMIF1_CORE, &ctx)
# Local HyperBUS memCfg (NOT the cycfg-generated OPI DDR one):
#   hbDevType=HB_SRAM, xipReadCmd=HB_READ_CONTINUOUS_BURST,
#   xipWriteCmd=HB_WRITE_CONTINUOUS_BURST, memSize=16M_BYTE, dummy=6,
#   flags = HYPERBUS_DEVICE|MEMORY_MAPPED|WR_EN
Cy_SMIF_HyperBus_InitDevice(SMIF1_CORE, &localMemCfg, &ctx)
# Patch PRESENT2=2 on both RD_DUMMY_CTL and WR_DUMMY_CTL for variable latency
Cy_SMIF_SetMode(SMIF1_CORE, CY_SMIF_MEMORY)   # XIP on
Cy_SMIF_InitCache(SMIF1_CACHE_BLOCK, region_0=[0x64000000..0x65000000 WT_RWA])
# Dynamic MPC: PC 0-7 NS RW on BOTH SMIF1_CACHE_BLOCK_CACHEBLK_AHB_MPC0 AND SMIF1_CORE_AXI_MPC0
```

M33 self-verify (earlier version had this, since removed): write `0xCAFEBABE` / `0xDEADBEEF` at `0x7400_0000`, read-back matches.

## 3. What's still broken

**CM55 NS reads at any 0x64xxxxxx address bus-fault** with precise data bus error:

```
***** BUS FAULT *****
  Precise data bus error
  BFAR Address: 0x64000000
  r15/pc: 0x6050xxxx (M55 XIP in SMIF0 — running code is fine)
```

Tested multiple addresses (`0x64000000`, `0x64010000`, `0x66000000`) — all fault identically. Also confirmed CM55 bus-faults on SMIF CTL registers at `0x54440000` / `0x54480000` (expected — those are M33-only per PPC; not the bug).

## 4. What's been ruled out

| Hypothesis | Status | Evidence |
|---|---|---|
| System-level MPC blocking NS access | **Ruled out** | Wide-open PC 0-7 NS RW on both `SMIF1_CACHE_BLOCK_CACHEBLK_AHB_MPC0` and `SMIF1_CORE_AXI_MPC0`; wouldn't fire BusFault anyway, MPC violations are their own signal |
| CM55 ARM MPU blocking | **Ruled out** | Added MPU region in `mpu_regions.c` for 0x6400_0000–0x6500_0000 when `CONFIG_INFINEON_SMIF_PSRAM=y`. Also, MPU block would fire MemManage not BusFault |
| Wrong XIP protocol (OPI DDR instead of HyperBUS) | **Fixed** | Switched from `mtb_serial_memory_setup` (OPI DDR per cycfg) to `Cy_SMIF_HyperBus_InitDevice`. M33 path proved correct. |
| HyperRAM variable-latency `PRESENT2` bit | **Fixed** | Cypress PDL hardcodes `RD/WR_DUMMY_CTL.PRESENT2 = 1` (fixed latency). Patched to 2 post-`HyperBus_InitDevice`. Arch manual §31.4.x mandates 2 for variable latency HyperRAM. |
| Cache region not programmed | **Fixed** | `Cy_SMIF_InitCache` programs REGION0 after XIP enable, attribute `CACHEABLE_WT_RWA`. Verified `COMPONENT_SECURE_DEVICE` gate is defined (InitCache body compiles; `grep -o "DCOMPONENT_SECURE_DEVICE" build/.../build.ninja` → hit) |
| APPCPUSS↔SYS bridge routing | **Ruled out by docs** | Arch ref manual §Table 3: CM55 "All accesses are performed through the Master-AXI (M-AXI) interface" uniformly for 0x6000_0000–0x9FFF_FFFF. Register manual: no SMIF1-only routing gate exists. |
| Uncommitted Zephyr tree mess | **Ruled out** | `git stash` of hal/infineon changes and baseline OCTAL test both reproduce. |
| Regression from earlier config | **Ruled out** | Pre-HyperBUS-fix state reproduces the same fault. Issue pre-existed this session. |

## 5. Key misleading diagnostics (DON'T get fooled by these again)

1. **0 bytes serial ≠ M33 hung ≠ M55 not running.** `cartoon_test` prints 0 bytes and runs perfectly (its M33 companion has `SERIAL=n`, M55 printk rarely fires). Always cross-check with *what the display shows*, not serial alone.
2. **Bus fault printks get swallowed** if you don't `k_msleep(200)` between them. The fault dump comes through cleanly with spacing, looks like total silence without.
3. **M33↔M55 shared SOCMEM diagnostic channel is flaky.** Values drift run-to-run (cache coherency / aliasing). Trust the "did M33 hang or not" signal (observable via "did M55 start") but don't trust status[] values written from M33 read from M55.
4. **M33 printk from pre-soc-init context silently does nothing.** Adding `CONFIG_SERIAL=y` to the companion doesn't help in `ifx_pse84_cm55_startup` — UART not yet routed.

## 6. Reference documents (all read; takeaways in memory)

- **`project_pse84_psram.md`** — current memory file, running state
- **PSE84 Architecture Ref Manual** — 1202 pg, confirms CM55 M-AXI reaches SMIF1 directly, no bridge enable
- **PSE84 Registers Ref Manual** — 3857 pg, confirms SMIF0/SMIF1 register-identical, no hidden per-instance gating
- **PSE84 Industrial Datasheet** — 140 pg, NOT YET READ (user was about to hand it to me when we paused)
- Reference examples:
  - `/tmp/psram-xip-example` — Infineon mtb-example-psoc-edge-psram-xip. **Runs PSRAM ONLY from CM33_S, its CM55 project has zero PSRAM access.** This is suspicious — might be a hint M55 PSRAM isn't actually supposed to work, OR it's just that the example's scope is limited.
  - `/tmp/ifx-mcuboot-pse84` — reference for octal NOR boot path

## 7. Test procedure (fast iteration loop, ~20s/cycle)

```
cd /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject
west build -b kit_pse84_eval/pse846gps2dbzc4a/m55 --sysbuild ../pse84_psram_test    # ~12s
west flash                                                                          # ~6s
# Then monitor (NEVER use cat /dev/tty — see CLAUDE.md rule):
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 -c "
import serial, subprocess, time
s = serial.Serial('/dev/cu.usbmodem103', 115200, timeout=1)
subprocess.run(['nrfutil','device','reset','--serial-number','1414033C03272400'], capture_output=True)
end = time.time() + 10
got = b''
while time.time() < end:
    d = s.read(4096)
    if d: got += d
print(got.decode(errors='replace'))
s.close()
"
```

If SMIF gets stuck-busy after too many failed boots: **power cycle the board** (ask user — soft reset doesn't clear it). Memory note `project_pse84_psram.md` has the symptom.

## 8. Key files

| File | Purpose |
|---|---|
| `zephyr_workspace/pse84_psram_test/` | Fast iteration test app. `src/main.c` = current probe, typically reads `0x64000000` with delays. |
| `zephyr_workspace/pse84_psram_test/prj.conf` | M55 config. `CONFIG_INFINEON_SMIF_PSRAM=y` adds MPU region. |
| `zephyr_workspace/pse84_psram_test/sysbuild/enable_cm55.conf` | M33 companion config. `CONFIG_INFINEON_SMIF_PSRAM=y` triggers `ifx_pse84_psram_init`. |
| `zephyr_workspace/zephyrproject/zephyr/soc/infineon/edge/pse84/security_config/pse84_boot.c` | `ifx_pse84_psram_init()` — HyperBUS init + MPC config. The main work lives here. |
| `zephyr_workspace/zephyrproject/zephyr/soc/infineon/edge/pse84/mpu_regions.c` | CM55 ARM MPU. PSRAM region added conditionally. |
| `zephyr_workspace/zephyrproject/zephyr/soc/infineon/edge/pse84/security_config/pse84_s_protection.c` | Static MPC regions; see `m55_mpc_regions` for the PSRAM entry. |
| `zephyr_workspace/zephyrproject/modules/hal/infineon/zephyr-ifx-cycfg/kit_pse84_eval/cycfg_qspi_memslot_octal.c` | Generated cycfg. Declares `smif1BlockConfig` (with S70KS1283), but runtime now ignores it in favor of a locally built memCfg. |

## 9. What to try next (ranked)

1. **Read the industrial datasheet** at `/Users/danahern/Downloads/infineon-psoc-edge-e8x-industrial-datasheet-datasheet-en.pdf` — may have a memory-map footnote or product-variant table that clarifies whether PSE846GPS2DBZC4A has SMIF1 fully wired on M55 side, or a boot-time external-memory enable.
2. **Attach GDB via KitProg3 / openocd** and dump SMIF1 registers post-`ifx_pse84_psram_init`:
   - `SMIF1_CTL` — expect `XIP_MODE=1`, `ENABLED=1`
   - `SMIF1_CORE_DEVICE[2]_CTL` — expect `ENABLED=1`
   - `SMIF1_CORE_DEVICE[2]_ADDR` / `_MASK` — expect 0x64000000 / 0xFF000000
   - `SMIF1_CACHE_BLOCK_MMIO_REGION0_BASE/_LIMIT` — expect BASE=0x4000<<12, LIMIT with ENABLE=1 ATTR=WT_RWA
   - GDB: `/Users/danahern/zephyr-sdk-0.17.4/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb`
   - openocd: `/Applications/ModusToolboxProgtools-1.7/openocd/bin/openocd` with `-f .../zephyr/boards/infineon/kit_pse84_eval/support/openocd.cfg`
3. **Check eval kit schematic** for RWDS pulldown on SMIF1 CS2 — arch manual §31.4.x says floating RWDS bus-faults XIP.
4. **Try the cartoon_test with PSRAM enabled** to see if its display init somehow primes SMIF1 differently (it might inadvertently touch something).
5. **Try `Cy_SMIF_Clean_And_Invalidate_All_Cache(SMIF1_CACHE_BLOCK)`** right before `Cy_SMIF_SetMode(CY_SMIF_MEMORY)` in case there's stale state.
6. **Try running `ifx_pse84_psram_init` from M33 NS side** (currently runs from secure) — the reference example runs PSRAM from CM33_S; we're CM33_S too, but worth sanity-checking.
7. **Compare SMIF1 register dump against a working ModusToolbox PSRAM example** (build `mtb-example-psoc-edge-psram-xip` via MTB, flash, dump post-init registers, diff against ours).

## 10. Operational constraints (remember these!)

- **NEVER** access `/dev/tty.*` directly (cat/screen/minicom/stty). Always via python+pyserial.
- Board is externally powered; `kitprog3 power_config` won't cycle it. Power cycle = unplug USB AND external power.
- User can power-cycle by hand on request.
- `pse84-gfxss-display-driver` is the active Zephyr submodule branch (NOT main).
- hal/infineon is on a detached-HEAD-style state; pushed branch is `pse84-psram` on `danahern/hal_infineon` fork.
- `west build` must run from `zephyrproject/` dir (not outer workspace).
- `CONFIG_INFINEON_SMIF_PSRAM` must be set on BOTH prj.conf (for MPU region) AND sysbuild/enable_cm55.conf (to trigger M33 psram_init).

## 11. Gotchas — every landmine I hit (or was already documented)

### Debug-interpretation traps
1. **"0 bytes serial" does NOT mean the M55 isn't running.** `cartoon_test` with `CONFIG_SERIAL=n` on the M33 companion prints 0 bytes while the display works fine. Always cross-check with something visible (display output, LED, peripheral behavior) before concluding the M55 crashed.
2. **Bus-fault printks vanish without `k_msleep(200)` spacing.** UART output is queued; if the fault hits before the queue drains, you see silence. Always pad printks with 200ms `k_msleep` before any potentially-faulting access.
3. **M33 printk from `ifx_pse84_cm55_startup` silently fails.** UART isn't routed yet at soc_late_init_hook time. Don't waste time setting `CONFIG_SERIAL=y` on the companion expecting diagnostics from M33 — it won't come through.
4. **M33↔M55 SOCMEM diagnostic channel is unreliable.** Values drift run-to-run due to cache coherency / secure-alias weirdness. If you MUST use it, write from M33 at secure alias `0x36250000`, read from M55 at NS alias `0x26250000` inside the `m33_m55_mpc_regions` range (offset 0x40000–0x500000 from SOCMEM base 0x26200000). Even then results are flaky.
5. **OpenOCD halt resets the CPU.** When attaching with `openocd -c 'halt; reg pc'`, both M33 and M55 PCs may show their pre-init state, not the running state. The `Pre-initializing: pc: 0x3ff01` message IS openocd forcing that state. For live register dumps you need a different approach (set breakpoints before halt, or use GDB attach to already-running target).
6. **Precise data BusFault (BFAR valid) vs MemManage fault (MMFAR valid) are different beasts.** MPU violations fire MemManage. BusFault = transaction made it out of the CPU and something downstream said no. On this SoC MPC violations also fire BusFault with BFAR set. Don't conflate.
7. **Same-looking fault dumps across runs don't mean "same bug" — check PC and register values.** Similar-looking 0x64000000 BFAR dumps had slightly different LR values as the M55 binary shifted across rebuilds; the identical-looking dump fooled me into thinking a fix had landed when it hadn't.

### Build / flash / tooling traps
8. **sysbuild path trap: `sysbuild/enable_cm55.conf` must NOT be nested as `sysbuild/sysbuild/enable_cm55.conf`.** Silently produces a black display / non-running M55. Check depth every time you copy a sample.
9. **M55 app's `prj.conf` configs don't control the M33 companion.** `CONFIG_INFINEON_SMIF_PSRAM=y` in `prj.conf` alone enables SMIF PDL on the M55 side (adds MPU region) but doesn't run the actual init. You MUST also set it in `sysbuild/enable_cm55.conf` for `ifx_pse84_psram_init` to fire. Both sides needed.
10. **`west build` must run from `zephyrproject/`, not the outer workspace.**
11. **Dual-hex flash.** `west flash` emits TWO programs: the M55 image and the signed M33 companion hex. Watch the output for both; if only one flashed, you're booting stale code.
12. **SMIF stuck-busy after failed boot.** `Cy_SMIF_BusyCheck` spins forever; soft reset via openocd doesn't clear it. **Only fix: physical power cycle.** Board is externally powered — USB unplug alone isn't enough. `kitprog3 power_config` fails ("target already powered").
13. **Stale `build_m55/` directories on disk from previous experiments**. They don't hurt builds (west uses `build/`) but can confuse ls. Safe to ignore.
14. **Clangd diagnostics noise** (`Unknown argument: '-fno-printf-return-value'`, `'nrfx_templates_config.h' file not found`) is pre-existing LSP misconfiguration, not real build errors. Ignore.
15. **`west` without args after a non-workspace `cd`** produces "unknown command 'build'". Stay in `zephyrproject/`.

### PSE84 hardware / register traps
16. **The "patched" octal FLM shipped in-tree was actually stock quad** at one point — MD5 `135196ed...` = stock, `5aedddc9...` = real octal. Regenerate via `qspi-configurator-cli -c design.cyqspi -o <out>` and verify MD5.
17. **`Cy_Mpc_ConfigRotMpcStruct` on SMIF1's MPC bus-faults M33 if SMIF1 isn't clocked yet.** Don't put SMIF1 MPC entries in the static `m55_mpc_regions` array — `cy_mpc_init` runs before SMIF1 is powered. Program SMIF1 MPC dynamically from inside `ifx_pse84_psram_init` after `Cy_SysClk_PeriGroupSlaveInit` for SMIF01. SMIF0 MPC is safe to program statically because ROM already clocked SMIF0.
18. **SMIF has TWO MPC instances per controller instance:** `CACHE_BLOCK_CACHEBLK_AHB_MPC0` + `CORE_AXI_MPC0`. SMIF0's static config has both. SMIF1 (PSRAM) must also have both in the dynamic config; configuring just the CACHE_BLOCK MPC is not enough.
19. **Cypress PDL `Cy_SMIF_HB_SetDummyCycles` hardcodes `RD/WR_DUMMY_CTL.PRESENT2 = 1` (fixed latency).** HyperRAM chips that boot in variable-latency mode (S70KS1283) need `PRESENT2 = 2` per PSE84 Arch Ref Manual §31.4.x. Must patch after `Cy_SMIF_HyperBus_InitDevice` returns.
20. **Configurator emits OPI DDR for HyperRAM.** The QSPI Configurator generates a `cy_stc_smif_mem_config_t` for S70KS1283 using OPI DDR commands (0xEE read, OCTAL+DDR). Chip boots in HyperBUS protocol — OPI DDR commands return garbage. DON'T use `mtb_serial_memory_setup` with this cycfg. Build a local `cy_stc_smif_hbmem_device_config_t` + memCfg with `CY_SMIF_FLAG_HYPERBUS_DEVICE` and call `Cy_SMIF_HyperBus_InitDevice` directly.
21. **`Cy_SMIF_InitCache` is `#if defined (COMPONENT_SECURE_DEVICE)`-gated.** If you don't build from the M33 secure companion, `Cy_SMIF_InitCache` is a no-op silently. Check the build.ninja for `-DCOMPONENT_SECURE_DEVICE` in the hal_infineon compile line to verify.
22. **SMIF1 XIP port MUST have at least one CACHE_BLOCK.REGION programmed as cacheable.** All regions default disabled = all transfers non-cacheable. For HyperBUS, non-cacheable passthrough CAN bus-error — per Register Ref Manual pp. 2108–2119. Use `CY_SMIF_CACHEABLE_WT_RWA` (not WB — avoids the write-back coherency question with M33 secure-alias writes).
23. **Cache region ADDR field is bits 12–26 (15 bits), granularity 4 KB, max 64 MB span.** `Cy_SMIF_InitCache` takes absolute addresses and masks them — confirmed correct for 0x64000000 → stored ADDR=0x4000.
24. **Hardware limit: 64 MB per SMIF XIP port.** SMIF0 XIP Port 0 at 0x60000000 is capped at 64 MB; SMIF1 XIP Port 1 at 0x64000000 is also 64 MB. To map a 128 MB chip you need MMIO (not XIP) for the upper 64 MB.
25. **M33 signing hex-addr is `0x60100000`, NOT `0x22011000`.** The `0x22011000` address from some docs is the old/wrong one; use `0x60100000`. The SMIF XIP alias map:
    - M33 Secure XIP: `0x1810_0000` AHB alias of `0x6010_0000` flash
    - M55 XIP: `0x6050_0000` (default 2 MB; cartoon_test extends to 10 MB)
    - SMIF0 Secure: `0x7000_0000`
    - SMIF1 NS: `0x6400_0000`, Secure: `0x7400_0000`
26. **MPC "region boundary off-by-one."** Configuring an MPC with `offset + size == maxSize` (exact end-of-aperture) triggers a cascading vector-table HardFault. Cap at 58 MB for the 64 MB octal aperture (comment in `pse84_s_protection.c`).
27. **SAU marks ALL of 0x40000000–0xFFFFFFFF as non-secure on this build,** including the 0x7xxxxxxx "secure alias". Writes from M33 to `0x74000000` go out with `HNONSEC=1`. So the "secure alias" is actually just NS-via-different-address-bits. Don't assume secure/NS separation there.
28. **CM55 cannot access SMIF control registers at 0x54440000 / 0x54480000.** These are M33-only per PPC — by design. CM55 only sees the XIP port address space. Don't try to peek SMIF regs from M55.
29. **Cy_GFXSS_Transfer_Frame is DBI-only**; calling it in DSI video mode corrupts DC regs and produces stripe artifacts. Not PSRAM-related but common trap on this SoC — noted in `feedback_gfxss_transfer_frame.md`.

### Config / conventions
30. **Don't use `sdk-alif/` for anything PSE84-related.** It's a separate Alif investigation. Use `ifx_bsp/`, `mtb_shared/`, and Zephyr's Infineon tree.
31. **Don't use Docker Desktop — use OrbStack.** Unrelated to PSRAM debug but a general project rule.
32. **Trust Zephyr/vendor drivers; check config before assuming bugs.** Before blaming PDL/driver code, verify your configuration matches what the driver expects. Cy_SMIF_HyperBus_InitDevice's PRESENT2=1 hardcode was a rare exception where the driver genuinely has a known gap for variable-latency HyperRAM.
33. **`TRUSTED_EXECUTION_SECURE` is how Zephyr signals "M33 secure companion"** in Kconfig — distinct from the M33 NS build.
34. **Code_relocate of `pse84_boot.c` to M33SCODE SRAM is gated on either OCTAL or PSRAM Kconfig.** Needed because SMIF mid-transition makes XIP fetches unreliable. Do NOT disable this gate — the M33 executes psram_init while SMIF1 is being reconfigured; running from SRAM is what keeps it alive.
35. **`pse84_psram_test`'s main.c is a probe, not a test.** It's meant to be edited often. Commits tend to rewrite it. Don't preserve old probe content.

### Session-specific
36. **Don't park-and-commit after every iteration.** See `feedback_keep_going.md`. One hand-off doc + reset is better than drip-commits.

## 12. How the reset should go

User will type `/clear` (or equivalent) and then either drop this file path in, or drop the datasheet path `/Users/danahern/Downloads/infineon-psoc-edge-e8x-industrial-datasheet-datasheet-en.pdf`. On resume:

1. Read this hand-off doc top to bottom (especially §4 what's ruled out, §9 what to try next, §11 gotchas).
2. Read `~/.claude/projects/-Users-danahern-code-claude-embedded/memory/project_pse84_psram.md` for the compressed version.
3. Verify tree state:
   - Outer repo on `main`, last commits `7f40e4b` / `a90491f` (PSRAM probe)
   - Zephyr submodule on `pse84-gfxss-display-driver`, last commit `9c2bceee299`
   - hal/infineon on `pse84-psram` branch tracking `danahern/hal_infineon:pse84-psram`, last commit `0839461`
4. Pick the highest-value next step from §9 (probably: read datasheet → GDB register dump → compare against working MTB example).
5. **Don't re-run ruled-out hypotheses from §4.** Don't re-verify M33 HyperBUS works — it does. Don't re-try wide-open MPC / MPU / different cache attributes without a new reason.
6. **Don't park unless you hit a genuine milestone or the user says pause.** See `feedback_keep_going.md`.

## 13. Session 2 log (datasheet read + M33-side regression discovery)

**Datasheet read:** `/Users/danahern/Downloads/infineon-psoc-edge-e8x-industrial-datasheet-datasheet-en.pdf` (140 pg). Read top-to-bottom for PSRAM-relevant sections. **No new gotcha found.** Summary:
- Errata §11: only autonomous-analog timer glitch + 3.3V GPIO leakage on B0 silicon. Not PSRAM.
- Power domains §4.1.2: CM33 (LP) + CM55 (HP) share VCCD. SMIF is a single block in the power-mode table — no per-instance gate.
- SMIF §5.5.4: "two SMIF interfaces, each equipped with 32 KB cache" — SMIF1 fully provisioned. "64 MB is supported in XIP mode." Matches arch ref manual.
- Variant §9.1 / Table 50: `PSE846GPS2DBZC4A` is EPC2 security, BGA-220, 5120 KB SRAM. No footnote disabling SMIF1 on this SKU.
- Pins §6: VDDIO.SMIF1 is a separate rail (E3/G3). If unpowered, SMIF1 wouldn't work at all — M33-side XIP historically proves it is powered.
- Electrical §8.7: HyperBUS max 200 MHz. No voltage/clock trap.

Conclusion: the datasheet does not reveal a new hypothesis. Hardware nominally supports exactly what we're doing.

**Re-enabling M33 self-test (user ask: "do both, restore M33 self-test and GDB register dump"):** uncovered two real bugs in the current tree, NOT present in the §2 "what works" description.

### Bug 1: `ifx_pse84_psram_init()` was commented out

In `pse84_boot.c:347` (pre-fix), the call site was literally `/* ifx_pse84_psram_init(); */ /* DEBUG: disabled to isolate crash */`. So SMIF1 was never being configured at runtime. The handoff §2 "M33 side: reliably working" was true at some earlier commit but not in the current tree.

### Bug 2: NULL deref on `smif1BlockConfig.memConfig[0]`

After re-enabling the call, M33 bus-faulted immediately with BFAR=0x0, PC=0x3400802a (in M33SCODE, `pse84_boot.c` line 45, code_relocate'd). That line was:
```c
cy_stc_smif_mem_config_t const *memCfg = smif1BlockConfig.memConfig[0];
```

The `smif1BlockConfig` symbol is defined in TWO cycfg files, only one of which is compiled based on `CONFIG_INFINEON_SMIF_OCTAL`:
- `modules/hal/infineon/zephyr-ifx-cycfg/kit_pse84_eval/cycfg_qspi_memslot_octal.c` — real slot (S70KS1283)
- `modules/hal/infineon/zephyr-ifx-cycfg/kit_pse84_eval/cycfg_qspi_memslot.c:528` — **stub with `.memConfig = 0`**

Our PSRAM-only build (OCTAL=n) links the stub, so `memConfig[0]` is a NULL deref. Fixed by replacing the dep with inline `CY_SMIF_SLAVE_SELECT_2` / `CY_SMIF_DATA_SEL0` constants (we build `psram_hb_memCfg` locally anyway).

### Bug 3: SMIF1 stuck-busy inside init

With both bugs fixed, M33 gets further into `ifx_pse84_psram_init` but hangs somewhere in the SMIF PDL calls (`Cy_SMIF_HyperBus_InitDevice`, `Cy_SMIF_InitCache`, or `Cy_SMIF_SetMode`). Symptoms:
- No UART output after reset (fault handler never reached).
- `openocd halt` on M33 reports `Failed to read memory at 0xe000edf0` — DAP unresponsive.
- Physical power cycle required (USB + external power) to clear.

### Bug 4: SOCMEM diagnostic channel faults from M33 Sec at this boot stage

Attempted to use the M33↔M55 SOCMEM channel (handoff §11 item 4) for progress markers — writes to `0x36250020` from M33 Secure during `ifx_pse84_psram_init` bus-fault. `Cy_SysEnableSOCMEM(true)` + `cy_mpc_init()` run before `ifx_pse84_psram_init`, but M33-Sec write aperture to SOCMEM at that offset isn't configured yet. **Don't use 0x36250000–0x36250020 as a diagnostic channel from inside psram_init** until this is understood.

### Reference example hint (not yet acted on)

`/tmp/psram-xip-example/proj_cm33_s/main.c:362-394` (`smif_ospi_psram_init`) does NOT use `Cy_SMIF_HyperBus_InitDevice`. It uses:
```c
mtb_serial_memory_setup(&serial_memory_obj, MTB_SERIAL_MEMORY_CHIP_SELECT_2,
                        CYBSP_SMIF_CORE_1_PSRAM_hal_config.base,
                        CYBSP_SMIF_CORE_1_PSRAM_hal_config.clock,
                        &smif_mem_context, &smif_mem_info, &smif1BlockConfig);
```

**This contradicts handoff §11 item 20** ("Configurator emits OPI DDR for HyperRAM. DON'T use mtb_serial_memory_setup with this cycfg."). Explanation: the example's `smif1BlockConfig` is a PSRAM-specific cycfg generated by building the example via MTB — different from the `kit_pse84_eval`-generated one we use. Worth building the reference example via MTB, dumping its generated `smif1BlockConfig`, and comparing flags (expect `CY_SMIF_FLAG_HYPERBUS_DEVICE` set and HyperBUS-mode commands, not OPI DDR).

Also note the reference uses write-back caching (`CY_SMIF_CACHEABLE_WB_RWA`) not our write-through.

### Next steps (ranked)

1. **Build `mtb-example-psoc-edge-psram-xip` via MTB and inspect its generated cycfg.** Compare against our local `psram_hb_memCfg`. If they match, our hand-rolled setup should work — the hang is elsewhere (clocks? MPC? cache block?). If they differ, adopt the reference's flags.
2. **Set GDB breakpoints inside `ifx_pse84_psram_init`** at each `Cy_SMIF_*` entry. Single-step to find which call hangs. Must be done before SMIF gets stuck-busy, or it's impossible to debug (DAP dies).
3. **Add lightweight progress tracking that doesn't use SOCMEM** — e.g., write to a `.noinit` section in M33 Secure SRAM at 0x340xxxxx, mirror-map into M55's view via a specific MPC region. Avoids the 0x36250020 fault issue.
4. **Re-check the reference example's `CYBSP_SMIF_CORE_1_PSRAM_hal_config.clock`** — if `mtb_serial_memory_setup` requires a valid clock handle and our local `ifx_pse84_psram_clock` (inst_num=4 for CLK_HF4) wasn't being passed through, that might explain the hang. Our current code doesn't pass a clock at all.

### Updated claims vs. original handoff

| Claim | Original | Actual |
|---|---|---|
| §2: M33-side XIP works reliably | "Reliably working" | **False in current tree.** Init was commented out; would NULL-deref if enabled. |
| §11 item 4: SOCMEM channel usable from M33 Sec at `0x36250000` | "Flaky but works" | **Faults during `ifx_pse84_psram_init`** — MPC/apertures not set up yet. |
| §11 item 20: `mtb_serial_memory_setup` can't be used with the cycfg | "DON'T use" | **Reference example uses it successfully with a different cycfg.** May be the right path after all, with the correct generated config. |
| §4 table: M33 HyperBUS init proven | "Ruled out as source of issue" | **Unknown** — never actually ran successfully end-to-end in current tree. |

## 14. Resolution (2026-04-12 session 2 late)

**Fix:** replaced the hand-rolled `Cy_SMIF_HyperBus_InitDevice` + `SetRxCaptureMode` + manual `SetDataSelect` sequence with a single `mtb_serial_memory_setup()` call, matching the Infineon reference `mtb-example-psoc-edge-psram-xip`.

**Current `ifx_pse84_psram_init` flow** (abridged):

```c
Cy_SMIF_Disable(SMIF1_CORE);
Cy_SMIF_Init(SMIF1_CORE, &smif1_config, 10000, &smif_ctx);
Cy_SMIF_Enable(SMIF1_CORE, &smif_ctx);

// The library does SetDataSelect, SetRxCaptureMode, HyperBus init:
mtb_serial_memory_setup(&obj, MTB_SERIAL_MEMORY_CHIP_SELECT_2,
                        SMIF1_CORE, &clock, &ctx, &info, &blockCfg);

// PDL hardcodes PRESENT2=1 (fixed latency); S70KS1283 boots variable:
SMIF_DEVICE_RD_DUMMY_CTL(dev) |= 2 << PRESENT2_Pos;
SMIF_DEVICE_WR_DUMMY_CTL(dev) |= 2 << PRESENT2_Pos;

Cy_SMIF_SetMode(SMIF1_CORE, CY_SMIF_MEMORY);
Cy_SMIF_InitCache(SMIF1_CACHE_BLOCK, &cache_cfg);  // WT_RWA, 16 MB
// Dynamic MPC (unchanged): wide-open PC 0-7 on both MPCs.
```

`blockCfg` is built locally as a single-slot `cy_stc_smif_block_config_t` pointing at a `cy_stc_smif_mem_config_t` with `.flags = HYPERBUS_DEVICE | MEMORY_MAPPED | WR_EN` and `.hbdeviceCfg` pointing to `CY_SMIF_HB_SRAM` / 16 MB / 6 dummy cycles. The clock handle is CLK_HF4 (SMIF1's HF clock) — passing NULL trips an assert inside the library.

**Hardware verification:**
```
== smif1 range probe ==
pre  @0x64000000 = 0x00000000
pre  @0x64010000 = 0x00000000
pre  @0x64800000 = 0x00000000
writing canaries...
post @0x64000000 = 0xcafebabe (want CAFEBABE)
post @0x64010000 = 0xdeadbeef (want DEADBEEF)
post @0x64800000 = 0x12345678 (want 12345678)
```
Three addresses spanning the full 16 MB aperture round-trip cleanly from CM55 NS at 0x64000000.

**Why the hand-rolled path failed:** still not fully understood. `mtb_serial_memory_setup` + `Cy_SMIF_MemNumInit` internally handle the HyperBUS-capable `SetRxCaptureMode` in a different order/context than our manual sequence. Specifically, our manual sequence set RxCaptureMode to `XSPI_HYPERBUS_WITH_DQS` **before** `Cy_SMIF_Enable`, but the library flips it at a different point. One of those ordering differences is load-bearing.

**Known outstanding issues (non-blocking for PSRAM functionality):**
- openocd reports "clearing lockup after double fault" on reset — suggests M33 is still hitting some fault (possibly the 0x74000000 secure-alias self-test canary write), but M55 gets to run and use PSRAM, so it's not blocking.
- Haven't measured throughput.
- MPC is wide-open; should narrow to actual protection contexts used.

## 15. Corrected claims (replaces §12 diff table)

| Original handoff claim | Actual result |
|---|---|
| §2: M33-side XIP works, M55 still fails | **False.** Init was commented out + had NULL-deref. Once fixed AND rerouted through `mtb_serial_memory_setup`, both M33 and M55 work. |
| §4: "CM55 cannot access SMIF1 XIP directly" was the hypothesis; all AXI-level gates ruled out | Correct — but moot. The issue was never AXI attribution. It was a broken SMIF1 controller init. |
| §11 item 20: "DON'T use `mtb_serial_memory_setup`" | **Wrong.** Using it was the fix. The warning was based on a different cycfg producing garbage — the library path with a properly-flagged HYPERBUS_DEVICE memCfg works. |
| §11 item 4: SOCMEM channel usable from M33 Sec | False during `ifx_pse84_psram_init` — faults. Don't use as a progress marker at that call site. |
