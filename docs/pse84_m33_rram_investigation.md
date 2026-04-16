# PSE84 M33 RRAM Boot Fault — Ongoing Investigation

Running log of what we know, what we've tried, what we've learned from the
Infineon architecture reference manual (docs/infineon/…-architecture-
reference-manual-…pdf) and on-chip experiments.

Last updated: 2026-04-14 (session continuation).

---

## Problem statement

M33 Zephyr-Secure images we build (pse84_assistant_m33) and the known-
working pse84_display project both hardfault when flashed into RRAM at
0x22011000 and handed off by extended-boot.

Byte-level flash pipeline (`tools/flash_m33_rram.sh`) works —
`edgeprotecttools image-metadata` produces the right header, openocd
verifies bytes land at 0x22011000. Extended-boot reads it back, but the
resulting M33 execution hardfaults.

With `policy_oem_octal.json` (current), extended-boot still releases CM55
itself, so M55 voice assistant runs fine even with M33 dead. With
`policy_oem_provisioning_filled.json`, M55 stays in CPU_WAIT because
extended-boot expects the M33 image to call `Cy_SysEnableCM55()` — our
M33 image hardfaults before reaching that call, so CM55 never starts.

---

## Memory map — corrected (arch ref §2.2.6, Table 4)

**Our overlay's addresses are all SRAM-region aliases** (S-AHB):
- `m33s_header` @ 0x22011000 → NS S-AHB RRAM
- `m33s_xip`    @ 0x32011400 → Secure S-AHB RRAM

RRAM is also aliased on the Code bus (C-AHB) — which ARMv8-M intends for
instruction fetch — at:
- NS  C-AHB: 0x02000000 – 0x0207FFFF (512 KB)
- Sec C-AHB: 0x12000000 – 0x1207FFFF

And the address `0x18100000` that earlier appeared in the memory note
(old image leftover in RRAM) is **SMIF0 Secure C-AHB XIP**, not internal
flash. The chip has no separate "internal flash" — it has RRAM (512 KB
NVM) and external SMIF0 (up to 64 MB NOR).

**Extended-boot default oem_app_address is `0x32011000/0x12011000`**
(arch ref §17.2.4.2.3 p.338 — both aliases listed, same physical RRAM).

---

## Arch ref §17.2.4.2.5 — Extended boot status in SRAM

Extended boot writes its state to CM33 SRAM on every run:

- `0x34000000` = boot mode (`IFX_BOOT_MODE_*` 0xAA000000…)
- `0x34000004` = error code (`IFX_ERR_*` 0xEE000000…)

Key codes for this fault:
- `0xAA000003 IFX_BOOT_MODE_LAUNCH_NEXT_APP` — control was handed to us
- `0xEE000011 IFX_ERR_VALIDATION_IMG_VECT_TBL_FAILED` — vector table invalid
- `0xEE00000E IFX_ERR_SEC_BOOT_VALIDATION_NOT_SUPPORTED` — reserved
- `0xEE000010 IFX_ERR_SEC_BOOT_VALIDATION_FAILED` — hash/sig/counter fail
- `0xEE000013 IFX_ERR_MAIN_BSP_INIT_FAILED` — HW init failed

First diagnostic run: **TODO** — halt with openocd, mdw 0x34000000 2.

---

## Byte dump currently in RRAM (before any new flash this session)

```
0x22011400: 3403ac48 1810933d 18110175 18109329 18109329 18109329 18109329 18109329
0x22011420: 00000000 00000000 00000000 18109371 18109329 00000000 181095b5 18109329
```

- word[0] initial MSP = 0x3403ac48 (SRAM top, reasonable)
- word[1] reset handler = 0x1810933d → **0x18109xxx is SMIF0 Secure XIP**

So whatever pse84_display build produced this (or a prior iteration) was
linked to **SMIF0 XIP** not RRAM — reset vector jumps into SMIF0, but our
policy doesn't enable SMIF0 (oem_app in RRAM), so the external NOR is
cold. Execution would immediately fault on instruction fetch into
un-clocked SMIF0 → that explains the hardfault observed before.

Our current pse84_assistant_m33 build (/tmp/m33_standalone) has
entry 0x3201a7a1 in the SBUS-Secure RRAM alias — should work IF
extended-boot is happy to launch from the SRAM-region alias.

---

## Hypotheses ranked

1. **Old RRAM image is the one we were debugging.** The current content
   in RRAM points reset handler into SMIF0 XIP. If our pipeline hasn't
   actually overwritten it with the pse84_assistant_m33 bytes, or if
   some step silently failed, then the "fault" we've been chasing is
   just the old pse84_display-from-SMIF0 image trying to XIP from an
   un-clocked SMIF0.
   → **Test**: reflash pse84_assistant_m33 via flash_m33_rram.sh, then
   mdw 0x22011400 8 and confirm word[1] is now in 0x3201xxxx.

2. **oem_app_start = 0x32011000 is SRAM-alias of RRAM — but extended-boot
   may prefer the Code-bus alias 0x12011000 for instruction fetch.**
   → **Test**: change policy oem_app_address to 0x12011000 and
   overlay to link m33s_xip at 0x12011400; reflash.

3. **SAU / Secure-state handoff mismatch.** We disabled the
   `ifx_pse84_cm55_startup()` path (CONFIG_SOC_PSE84_M55_ENABLE=n) which
   also disables `cy_sau_init()`. Extended-boot's SAU may be minimal
   and not cover regions our early code touches.
   → Only testable after (1) rules out the stale-image hypothesis.

---

## Timeline of this session

- Baseline OK: voice assistant running, audio_hb blks=3728 at 6min
  uptime, chip on policy_oem_octal.
- Built pse84_assistant_m33 standalone at /tmp/m33_standalone —
  entry=0x3201a7a1, text @ 0x32017718, vectors @ 0x32011400 (SBUS Sec).
- Halted via openocd, read 0x22011400 — reset vector is 0x1810933d
  → **SMIF0-Secure XIP**, not our build. So an older display build (or
  pre-overlay-fix build) is currently in RRAM.
- Read arch ref §2.2.6 memory-map table + §17.2.4.2 extended boot +
  §17.2.4.2.5 boot status codes.
- Flashed pse84_assistant_m33 to RRAM; still hardfaulted; VTOR after
  fault = 0x18100400 (SMIF0 Secure XIP), MMFAR/BFAR = 0x5240D008
  (SYSCPUSS peripheral secure region). Confirmed the fault happens
  **before** our __start runs.
- Rebuilt with C-AHB Secure overlay (0x12011400) — still faulted.
- Re-provisioned chip with fresh policy_oem_octal — still faulted.
- Probed with hw breakpoints at 0x1201a7a0, 0x3201a7a0 (our __start in
  both aliases) AND 0x1810943c (the SMIF0 image's reset handler).
- **Hit at 0x1810943c** → extended-boot was launching the stale SMIF0
  image, not ours. VTOR = 0x70100400 = `oem_alt_app_address` (arch ref
  §17.2.4.2.3 — alt-boot slot in SMIF0 S-AHB secure alias).

## Root cause (2026-04-14)

**`oem_alt_boot: true` in policy + P17.6 pulled high on kit_pse84_eval
= extended-boot always takes the alternate boot path to SMIF0.**

Arch ref §17.2.4.2.3 p.338:
> "When oem_alt_boot is enabled, the GPIO input P17.6 controls the boot
> location. If P17.6 is connected to VDD with a resistor of 5K or less,
> the extended boot code will boot from the address specified by
> oem_alt_app_start."

Our `policy_oem_octal.json` had `oem_alt_boot: true`,
`oem_alt_app_address: 0x70100000` (SMIF0 secure). The kit must have
P17.6 pulled high (Waveshare display connector or onboard pull-up); so
extended-boot always used the SMIF0 path, loading whatever stale OEM
image had been programmed there by prior experimentation. Our RRAM
image at 0x32011000 was never reached.

## Fix

1. `policy_oem_octal.json`: set `oem_alt_boot.value: false`
2. `edgeprotecttools ... provision-device -p policy_oem_octal.json ...`
3. Re-flash pse84_assistant_m33 to RRAM via `flash_m33_rram.sh`

After the fix:
- HW bp at 0x1201a7a0 (our __start in C-AHB Secure) hits on reset.
- VTOR moves from 0x32011400 (extended-boot handoff) → 0x12011400 once
  Zephyr's `relocate_vector_table()` runs.
- M33 reaches main() and prints `[m33] heartbeat tick=1` on uart2.

## Secondary finding — overlay alias choice

Either 0x12011400 (C-AHB Secure) or 0x32011400 (S-AHB Secure) as
`m33s_xip` reg works **once alt-boot is disabled**. Both are aliases
of the same physical RRAM; Cortex-M33 can instruction-fetch from either
on PSE84. The C-AHB alias is slightly more idiomatic (matches ARMv8-M
Code region 0x00000000–0x1FFFFFFF), but Zephyr's default S-AHB placement
also runs. Changing the overlay to C-AHB was NOT the load-bearing fix —
the alt-boot flag was.

## Still TODO

- Rebuild pse84_assistant (sysbuild M55 + M33 companion) so the voice
  assistant runs again — right now only the standalone M33 is in RRAM;
  M55 has no image at m55_xip.
- Confirm log tunnel binds (need M55 to register `assistant` ipc peer).
- Verify `bt_enable()` path (previous session was blocked; unrelated
  to this fix).

---

## 2026-04-14 evening — follow-on work

### Fix: sysbuild M33 overlay linked at SMIF0, not RRAM

`pse84_assistant/sysbuild/enable_cm55.overlay` had
`&m33s_xip { reg = <0x18100400 ...>; };` with a comment claiming that
"Cortex-M fetches instructions via CBUS — SBUS is data-only for code
regions on this SoC." That comment was wrong:

- 0x18100400 is **SMIF0 Secure C-AHB XIP** (external NOR flash), not
  RRAM at all (arch ref §2.2.6 Table 4).
- The RRAM C-AHB Secure alias is 0x12000000–0x1207FFFF; RRAM S-AHB
  Secure is 0x32000000–0x3207FFFF. Both are valid for code.

The linked-at-SMIF0 image silently "worked" only because
`oem_alt_boot=true` + P17.6-high was making extended-boot jump to
`oem_alt_app_address=0x70100000` (SMIF0 alt slot), running whatever
stale bytes had been programmed there by prior `west flash` runs. Once
the alt-boot was disabled the real address mismatch surfaced: the
bytes the script writes live in RRAM, but the linker produced an
image that expected to XIP from SMIF0.

Fixed in commit on pse84-voice-assistant by restoring the overlay to
`&m33s_xip { reg = <0x32011400 DT_SIZE_K(356)>; };`.

### Missing M55 IPC config on M55 prj.conf

`pse84_assistant/prj.conf` was missing CONFIG_MBOX / CONFIG_IPC_SERVICE
/ CONFIG_IPC_SERVICE_BACKEND_ICMSG. The M33 companion config had them
but M55's side of the ipc tunnel (log_tunnel.c) linked against
undefined `ipc_service_send`. Added the three kconfigs to M55 prj.conf.

### Fix: ui_reply_active outside CONFIG_APP_ANIMATION_PROCEDURAL

`src/ui.c` had `static bool ui_reply_active;` gated inside the
PROCEDURAL `#ifdef` but the consumers (`ui_append_reply_text` /
`ui_clear_reply_text`) are unconditional. With the current
CONFIG_APP_ANIMATION_SPRITES build the symbol was undeclared. Moved
the declaration outside the `#ifdef`.

### CM55 release under policy_oem_octal

Earlier project memory claimed "policy_oem_octal: extended-boot
releases CM55 itself." Empirically wrong — after the alt_boot fix,
CM55 stays at PC=0x0003ff00, SP=0x100 (CPU_WAIT) unless the M33
explicitly calls Cy_SysEnableCM55().

### `CONFIG_SOC_PSE84_M55_ENABLE=y` bus-faults on re-running MPC init

Enabling `ifx_pse84_cm55_startup()` in soc_late_init_hook crashes
with a precise bus fault at `0x54463000` (APPCPUSS peripheral secure
region) during `cy_mpc_init` → `Cy_Mpc_ConfigRotMpcStruct`. LR points
into `m33s_mpc_cfg` at `0x32026157`; PC=0x00000001 (call-through-NULL).
Extended-boot has already attributed these regions per policy, so the
re-run writes registers that aren't clocked/accessible.

Attempted fix: patch pse84_boot.c to gate `cy_mpc_init()` and
`cy_ppc_init()` behind a new Kconfig `INFINEON_EXTENDED_BOOT_DID_MPC`.
That cleared the MPC fault but surfaced a downstream bus fault at
`BFAR=0x240fe004` (ipc_tx shared-memory region) — the MPC gating left
our Secure M33 without rights to its own IPC shared RAM. Reverted.

### Current workaround: manual release from main()

`CONFIG_SOC_PSE84_M55_ENABLE=n`; M33's `main()` calls `release_cm55()`:
Cy_System_EnablePD1 + peri-group init + Cy_SysEnableSOCMEM +
Cy_SysEnableCM55. Register probes show post-release:
- CM55_CTL=0 (CPU_WAIT cleared)
- CM55_CMD self-cleared RESET bit (reset pulse took)
- S_VEC=0x70500000 (Secure SMIF0 alias of m55_xip)
- STATUS=0 (=ACTIVE per the PDL define — CM55 awake)

Halting CM55 after 2s: openocd reports "clearing lockup after double
fault", PC=0xeffffffe, MSP=0xffffffe0. So CM55 is being released
correctly but immediately hits a fault and double-faults.

### Root cause (suspected): SMIF0 not XIP-configured when oem_app is in RRAM

Per arch ref §17.2.4.2.2: "The extended boot powers on and configures
the external flash only when the application launch address is
located within the external flash." Our `oem_app_address=0x32011000`
is RRAM, so extended-boot doesn't fully configure SMIF0 (only enough
for clock — evidence: 0x18500000 CBUS Secure returns real bytes, but
0x60500000 NS SBUS errors from openocd's M33 context). CM55 fetches
vectors OK (Secure CBUS) but hits a fault on subsequent XIP reads.

### Next attempt: move M33 image to SMIF0

Changed overlay to `m33s_header: 0x60100000`, `m33s_xip: 0x18100400`
(SMIF0 Secure code alias) and policy `oem_app_address: 0x18100000`.
Extended-boot then powers + XIP-configures SMIF0, and both cores can
XIP from it. Standard `west flash` places both images on SMIF0.
Re-provisioned 2026-04-15.

### Extended-boot rejects 0x18100000 as oem_app_address

Boot status 0x34000004 returned `0xEE000024 IFX_ERR_UNKNOWN_MEMORY_RANGE`
("Boot address of the next application belongs to unknown memory
range"). Arch ref §17.2.4.2.3 examples use `0x70100000` (SBUS Secure
SMIF0), not CBUS. Changed to `oem_app_address: 0x70100000` and
re-provisioned. That boots fine and extended-boot powers SMIF0.

### 2026-04-15 late — display lit ✓

**Fix:** flip `oem_alt_boot` back to `true` (was false). With P17.6
pulled high on kit_pse84_eval, extended-boot takes the alt-boot
path to `oem_alt_app_address=0x70100000` (set the same as
`oem_app_address=0x70100000` so either path lands on our M33
signed hex). The MPC state extended-boot leaves under alt_boot=true
is the one Zephyr's `ifx_pse84_cm55_startup()` expects —
`cy_mpc_init`/`cy_ppc_init` run cleanly instead of faulting on
APPCPUSS rows.

Also flipped `CONFIG_SOC_PSE84_M55_ENABLE=n` → `=y` in
`pse84_assistant/sysbuild/enable_cm55.conf` so the full
ifx_pse84_cm55_startup handles CM55 release. Deleted our hand-
rolled `release_cm55()` + MPC surgery from `pse84_assistant_m33/
src/main.c` — no longer needed.

**Why alt_boot=false broke things even though we had the oem_app
address right:** extended-boot's MPC setup differs by boot path.
alt_boot=true leaves APPCPUSS MPC configured for the already-
attributed layout Zephyr expects to re-write; alt_boot=false
locks those rows so `Cy_Mpc_ConfigRotMpcStruct` faults.

**Still TODO:**
- Post-flip, M33 uart output is silent past the Zephyr boot banner.
  Could be a side-effect of ifx_pse84_cm55_startup's `__disable_irq`
  (serial backend interrupt-driven) — verify and route M33 logs
  to polled mode if needed.
- Display lights but "not animating" per user observation. M55 may
  be faulting after GFXSS init — look for the M55 log stream via
  the ipc tunnel once M33 side is producing output.

### End-of-session status (2026-04-15 evening)

**Working:**
- Extended-boot launches our M33 image cleanly from SMIF0
  (`oem_app_address: 0x70100000`, `oem_alt_boot: false`).
- M33 Zephyr boot banner, IPC endpoint 'assistant' registers,
  heartbeats tick every 5 s.
- Mac host bridge end-to-end verified via `bridge.py
  --transport=file-inject --fake-transcript ... --ollama-model
  glm-4.7-flash:latest` — reply "Paris is the capital of France."
  streamed back cleanly.

**Not working — M55 voice assistant doesn't boot:**
- `release_cm55()` on M33 calls Cy_System_EnablePD1 + peri groups +
  SOCMEM + Cy_SysEnableCM55 + Cy_SysCM55Reset + direct CPU_WAIT
  clear. Register dumps post-release show CTL=0, CMD VECTKEYSTAT
  accepted (0xFA05 readback), S_VEC=0x70500000, NS_VEC=0x60500000.
- Halting CM55 reports PC=0x3ff00, SP=0x100 — CPU_WAIT pre-init
  state. openocd says "Clearing CPU_WAIT" on debug-attach, which
  obscures whether CM55 actually started.
- Switching to `CONFIG_SOC_PSE84_M55_ENABLE=y` to use the full
  `ifx_pse84_cm55_startup` faults M33 in `cy_mpc_init` at APPCPUSS
  MPC (0x54463000) — same fault observed on RRAM policy earlier.

**Suspected root cause:** extended-boot's MPC regions don't grant
CM55 access to SMIF0 XIP, and our minimal release skips the
cy_mpc_init that would fix this. cy_mpc_init itself faults on
already-configured APPCPUSS entries. Needs a surgical patch writing
only M55-facing MPC entries without touching the locked-down
APPCPUSS rows.

**Next steps:**
- Read M55 MPC entries directly from a known-good example
  (e.g. the Avnet voice-assistant reference in
  `/Users/danahern/mtw/Avnet_PSOC_Edge_DEEPCRAFT_Voice_Assistant/`)
  and apply only those in release_cm55.
- Alternative: drop M55 image down to a minimal bare-metal main
  that just writes a pattern to SOCMEM; poll that pattern from
  M33 to prove CM55 executed at all.
- BT/BLE/WiFi bring-up (Phase 4) is blocked on M55 running, since
  ble.c is on M55.

---

### openocd SMIF0 bank was capped at 16 MB (silent flash truncation)

Running chip booted a 3-commits-old image despite "wrote 11534336
bytes" success message. `flash banks` showed:
```
#2 : cat1d.cm33.smif1_ns at 0x60000000, size 0x01000000 (16 MB)
```
M55 ELF extends to 0x61D33E18 (~29 MB). openocd silently dropped
records with `:02000004610x...` upper-address. Chip ran the
stale-image leftover.

Fix: set env `SMIF_OCTAL=1` so `qspi_config.cfg` declares a 64 MB
bank at slot 0. Must pair with `SMIF_OCTAL_FLM=/path/to/patched-FLM`
pointing to
`zephyr_workspace/pse84_octal_enablement/cycfg_octal_generated/PSE84_SMIF.FLM`
(generated via MTB qspi-configurator-cli for S28HS01GT octal). With
both vars set, `flash banks` shows:
```
#2 : cat1d.cm33.smif0_ns at 0x60000000, size 0x04000000 (64 MB)
```
and the full M55 image writes.

Also included `kit_pse84_eval_octal_64mb.overlay` in pse84_assistant
M55 overlay so the Zephyr-side flash0 reg matches (64 MB vs stock
16 MB). Was: stock 16 MB flash0 + 40 MB m55_xip override = incoherent.

Disabled CONFIG_INFINEON_SMIF_PSRAM on the M33 companion for now to
keep the CM55 release path lean — the voice-assistant bring-up
doesn't need HyperRAM yet; can re-enable once everything else is
running.

---

## Session continuation (2026-04-16) — SOC hook split for IPC tunnel

**Context recap:** previous session ended with M55 voice assistant
animating cleanly (after `CONFIG_SOC_PSE84_M55_ENABLE=y` fix +
polled UART + sync log + sprite bundle disabled) but M33 silenced
back down to GPIO/PINCTRL/CLOCK_CONTROL only — every attempt to
re-enable SERIAL or MBOX bus-faulted M33 because driver probes at
PRE_KERNEL_1 / POST_KERNEL touch SCB2 / pse84_mbox **before**
`soc_late_init_hook` → `ifx_pse84_cm55_startup` runs PPC
re-attribution.

### Why "bump drivers to APPLICATION level" didn't work

First attempt: edit `drivers/serial/uart_infineon_pdl.c` and
`drivers/mbox/mbox_pse84.c` to use APPLICATION init level. Result:
compile errors in Zephyr's `DEVICE_DT_INST_DEFINE` macro chain —
`ZERO_OR_COMPILE_ERROR` rejects APPLICATION for device-class
drivers (Zephyr design choice; see `include/zephyr/init.h`).

### Working fix: split the SOC hook into early + late phases

`soc/infineon/edge/pse84/security_config/pse84_boot.c`:
- New `ifx_pse84_cm55_early_init()` — runs from `soc_early_init_hook`,
  BEFORE driver probes. Does SAU + SCB/NVIC/FPU + PD1 + peri groups +
  SOCMEM + `cy_mpc_init` + `cy_pd_pdcm_clear_dependency` + `cy_ppc0_init`
  + `cy_ppc1_init`. Removed the `__disable_irq` dance around `cy_ppc_init`
  — not needed in early phase because M55 isn't released yet.
- `ifx_pse84_cm55_startup()` — now ONLY does `ifx_pse84_psram_init` (if
  PSRAM) + `Cy_SysEnableCM55` + `Cy_SysPm_SetSOCMEMDeepSleepMode`. Runs
  from `soc_late_init_hook` after all driver probes complete.

`soc/infineon/edge/pse84/soc_pse84_m33_s.c`:
- `soc_early_init_hook` calls `ifx_pse84_cm55_early_init()` after
  `SystemInit()` (gated on `CONFIG_SOC_PSE84_M55_ENABLE`).
- `soc_late_init_hook` retains `ifx_pse84_cm55_startup()` call.

`soc/infineon/edge/pse84/security_config/pse84_boot.h`:
- Added `void ifx_pse84_cm55_early_init(void);` declaration.

### M33 companion re-enabled

`zephyr_workspace/pse84_assistant_m33/prj.conf`:
- Added `CONFIG_SERIAL=y CONFIG_CONSOLE=y CONFIG_UART_CONSOLE=y
  CONFIG_PRINTK=y` — uart now safe because PPC attribution happened
  in early-init.
- Added `CONFIG_MBOX=y CONFIG_IPC_SERVICE=y
  CONFIG_IPC_SERVICE_BACKEND_ICMSG=y` — IPC tunnel back online.

`zephyr_workspace/pse84_assistant_m33/src/main.c`:
- Restored full `ipc_service_open_instance` + `register_endpoint` flow
  for the `assistant` endpoint on `assistant_ipc0` mailbox node.
- `ep_recv` callback prefixes every received line with `[m55] ` and
  emits to printk (uart2). Tracks `at_line_start` across receives.
- 5 s heartbeat keeps a liveness signal on the wire.

### Trade-off

Zephyr boot-banner ("\*\*\* Booting Zephyr OS build … \*\*\*") happens
during PRE_KERNEL_2, before APPLICATION-level work. Even though the
uart driver itself is now safe, the banner uses cbprintf which goes
through console_init that still races early. Banner may not appear —
but `printk` from `main()` does, so M33's `=== PSE84 M33 companion ===`
line is the practical "boot proof".

### Next verification

Flash M55 + M33 (sysbuild outputs both), capture serial. Looking for:
1. `=== PSE84 M33 companion ===` — proves M33 main runs after
   driver attribution.
2. `[m33] ipc 'assistant' endpoint registered` — proves MBOX driver
   probe succeeded.
3. M55 LVGL animation visible on display — proves
   `soc_late_init_hook` → `Cy_SysEnableCM55` still releases M55
   correctly with `cy_ppc_init` moved earlier.
4. `[m33] ipc 'assistant' endpoint bound — log tunnel live` —
   proves M55 peer endpoint bound back.
5. `[m55] …` lines on uart — proves end-to-end log tunnel.

If M33 still silent → likely some driver still probes
pre-attribution; check Zephyr boot ordering for `MBOX_INIT_PRIORITY`
default vs the time `soc_early_init_hook` runs.

If M55 doesn't animate → moving `cy_ppc_init` earlier broke a
post-handoff invariant; back out of early-init and re-test with
attribution moved into late-init only (M33 silent again).

### 2026-04-16 SOC hook split result: M33 BusFault

**Flashed + reset with SOC hook split in place + M33 SERIAL/MBOX re-enabled.**

Result:
- Extended-boot: OK, launches M33 from SMIF0.
- M33 PC after 3 s: `0x1810f240` = `arch_system_halt` (Zephyr panic).
- MSP = `0x34038cb8`, mode = Handler BusFault, xPSR IPSR=5.
- Stack frame shows: `bus_fault` → `z_arm_fault` → `z_fatal_error` → `arch_system_halt`.
- CFSR = 0, BFAR = `0x34038cbc` (just past stack pointer — likely precise
  fault on some MPC-region struct access).
- Stack contains `0x18110134` which points INTO `m33_m55_mpc_regions`
  rodata — suggesting cy_mpc_init / cy_ppc_init struct iteration.
- `0x34000000 = 0xa0000003` (NOT `0xAA000003` = LAUNCH_NEXT_APP) —
  indicates M33 overwrote the boot status word with own SRAM data,
  which means M33 DID get past extended-boot.
- CM55 stuck at `0x3ff01` (CPU_WAIT) — M33 didn't reach
  `Cy_SysEnableCM55` in late-init before panic.

**Verdict:** Moving cy_mpc_init + cy_ppc_init to early-init phase
bus-faults somewhere in the MPC/PPC region walk. Likely causes:
- Some memory region the MPC config touches isn't powered on at
  early-init timing (POST_KERNEL is later, more things set up).
- IRQ state at early-init differs from late-init (we run with IRQs
  enabled; baseline disabled them before cy_ppc_init). Stale IRQ
  vector fetch during re-attribution?

**Decision:** revert SOC hook split. Accept the Phase 0b.1 constraint
(M33 minimal silent, no SERIAL/MBOX). Display animates from M55, and
v1 does not require M33 logs. Later solutions to investigate:
1. Fix the M33 post-handoff hardfault (cmt fae9ce777c6 added
   `__disable_irq` around cy_ppc_init but fault still occurs —
   needs deeper RCA per `project_pse84_m33_hardfault.md` memory).
   Once M33 runs main() cleanly, the IPC tunnel can light up.
2. Route M55 logs OUT via bridge.py directly over L2CAP once BLE
   is up (Phase 4 goal). Skip the M33 relay entirely.

### 2026-04-16 — baseline restored + verified animating

Flashed with:
- `pse84_boot.c` / `pse84_boot.h` / `soc_pse84_m33_s.c` clean (no split)
- `pse84_assistant_m33/prj.conf` = minimal silent (GPIO/PINCTRL/CLOCK_CONTROL only)
- `pse84_assistant_m33/src/main.c` = empty forever-idle main()

Result (openocd halt + inspect after 4 s reset):
- CM55: PC=`0x60546fa4` (in M55 image SMIF0 range), Thread mode — LVGL
  animation running. User confirmed "display animates".
- CM33: HardFault after double-fault ("clearing lockup after double fault"
  from openocd, PC=`0xeffffffe` = EXC_RETURN). Matches known-good
  per `project_pse84_m33_hardfault.md` memory — M33 hardfaults post-
  handoff, but CM55 already has control and display still works.
- Halting CM55 via openocd freezes animation (expected).

**Baseline = good for v1 voice-assistant display/audio work.**
Phase 4 BLE/WiFi still needs M33 running, but that's a separate
investigation (project_pse84_m33_hardfault.md).

### 2026-04-16 evening — REAL M33 hardfault root cause + BLE on M55

After deeper RCA (see embedded/docs/pse84_m33_secure_uart_fault.md):

**Root cause:** `cy_ppc_init` re-attributes ALL of PERI0 to NS, then
M33-Secure side accesses to peripherals via Secure aliases bus-fault.
- `0x529A0000` = SCB2 Secure alias (boot banner uart write)
- `0x52xxxxxx` = MCWDT Secure alias (kernel tick source via INFINEON_LP_TIMER)
- `0x52xxxxxx` = SCB4 Secure alias (would be HCI to CYW55513)
- `0x52xxxxxx` = IPC Secure aliases (mbox/icmsg)

The earlier RCA (M33_HARDFAULT_RCA.md / project_pse84_m33_hardfault.md
memory) had VECTTBL fault; that was a DIFFERENT manifestation of the
SAME issue (with SERIAL=n, idle path hit some other PERI0 peripheral
indirectly via NVIC). `__disable_irq` fix landed in fae9ce777c6
silenced THAT specific path but the underlying problem remained.

**Fix applied (Option A from the RCA):** disable M33 console + drop
`INFINEON_LP_TIMER_PDL` for `CORTEX_M_SYSTICK` (CPU-internal, not
PPC-affected). M33 now runs main() loop cleanly with `k_yield()`
(NOT `k_sleep` — that triggers a separate Zephyr timeout assertion
on the kernel tick path). IPC bind still doesn't complete (icmsg
handshake stalls), but the bind isn't strictly required for v1.

**BLE pivot:** Instead of fighting M33 PERI0 access, moved BLE host
to M55 NS (where peripheral access is clean). `kit_pse84_ai_m55.dts`
already shows the pattern; reused. Result: M55 advertises as
`PSE84-Assistant` over CYW55513 + uart4 HCI. ~18 s firmware download
(115200 baud, can be raised). Phase 4 unblocked.

**Net status (end of session 2026-04-16):**
- ✅ Display animates (M55 LVGL on GFXSS)
- ✅ Audio capture (PDM via DMIC, with FIFO overflow under BT load
  — needs priority tuning)
- ✅ BLE advertising (`PSE84-Assistant`, public addr 55:50:0A:1A:76:93)
- ✅ M33 boots, runs main() loop, no fault
- ⏳ M33 IPC bind — registered but never bound (workqueue/handshake)
- ⏳ WiFi — config attempted, M55 DTCM overflow (need WHD-to-SOCMEM
  link script change)
- ⏳ L2CAP CoC handler (next phase 4 step)
