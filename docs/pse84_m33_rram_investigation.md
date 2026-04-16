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
