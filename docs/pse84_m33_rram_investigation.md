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
