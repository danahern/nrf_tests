# PSE84 M33 Secure UART Fault — Root Cause + Fix Options

**Discovered:** 2026-04-16. **Status:** RCA complete, partial fix applied.

This is the actual root cause of the long-standing "M33 hardfaults
post-handoff" issue logged across multiple sessions. Earlier RCAs
(`zephyr_workspace/pse84_assistant/M33_HARDFAULT_RCA.md`,
`project_pse84_m33_hardfault.md` memory) hypothesized vector-table
fetch failure or NVIC ITNS issues — wrong. The truth is simpler and
more concrete.

---

## Symptom

After `ifx_pse84_cm55_startup()` returns from `soc_late_init_hook`,
M33 enters Zephyr's main thread setup. The boot banner ("\*\*\* Booting
Zephyr OS build … \*\*\*") triggers `printk` → `console_out` →
`ifx_cat1_uart_poll_out` → write to SCB2 register at the address the
driver computed from the device tree. M33 takes a precise BusFault,
escalates to HardFault, double-faults inside the HF handler, enters
LOCKUP. openocd reports:

```
Error: [cat1d.cm33] clearing lockup after double fault
```

CM55 is unaffected because `Cy_SysEnableCM55()` already ran before
the fault — but M33's `main()` never runs, so no IPC, no heartbeat,
no BLE host (Phase 4).

## Fault registers (precise)

```
PC=0xeffffffe  msp=0x340384a8        # EXC_RETURN, in HF handler after double-fault lockup
xPSR=0x29000003  IPSR=3 (HardFault)
CFSR=0x00008200  → BFSR.PRECISERR=1, BFSR.BFARVALID=1
HFSR=0x40000000  → FORCED=1 (escalation), VECTTBL=0
BFAR=0x529A0000  → faulting address
MMFAR=0x529A0000 (mirrored, MMFAR not relevant since MMFSR=0)
```

Stacked exception frame (offset from `msp + N*4`):
```
+0x00 R0  = 0x0000002a
+0x04 R1  = 0x1810d193
+0x08 R2  = 0x1810d185
+0x0C R3  = 0x0000002a
+0x10 R12 = 0x1810e098
+0x14 LR  = 0x181080c7  ← console_out at uart_console.c:111 (caller)
+0x18 PC  = 0x181080a9  ← console_out at uart_console.c:99 (faulting insn)
+0x1C xPSR= 0x80000000
```

`addr2line` chain:
- `0x181080a9` = `console_out` (`drivers/console/uart_console.c:99`) — invokes `uart_poll_out`
- `0x1810d193` = `ifx_cat1_uart_poll_out` (`drivers/serial/uart_infineon_pdl.c:327`) — writes SCB2
- `0x1810d185` = `ifx_cat1_uart_poll_out` (`drivers/serial/uart_infineon_pdl.c:324`) — same function

## What the BFAR address means

`0x529A0000` decodes as **SCB2 Secure alias**:

| Header source | Macro | Address |
|---|---|---|
| `pse846gps2dbzc4a.h` (NS device) | `SCB2_BASE` | `0x429A0000` |
| `pse846gps2dbzq3a_s.h` (Secure device) | `SCB2_BASE` | `0x529A0000` |

`pse84_config.h:7445`:
```
PROT_PERI0_SCB2 = 0x00000130u, /* Peri 0, Address 0x429a0000, size 0x00010000 */
```

So SCB2 is in PERI0. After `cy_ppc_init` re-attributes every PERI0
region to `NON_SECURE/NONPRIV` (`pse84_s_protection.c:274`,
`cycfg_unused_ppc_cfg`), the Secure-side alias is no longer accessible
from M33 Secure. The NS alias `0x429A0000` would still work — but the
M33 Zephyr build is Secure (`CONFIG_TRUSTED_EXECUTION_SECURE=y` from
`kit_pse84_eval_m33_defconfig`), so `<infineon/pse846gps2dbzq3a_s.h>`
is in scope and the driver uses `0x529A0000`.

## Why earlier RCAs got it wrong

The original `M33_HARDFAULT_RCA.md` (Phase 0a) saw `HFSR=0x40000002`
(`FORCED|VECTTBL`). That was with `CONFIG_SERIAL=n` on the M33
companion — no uart access at all, so the fault came from a different
path (probably an interrupt vector fetch into a re-attributed region).
The `__disable_irq` fix that landed (commit `fae9ce777c6`) silenced
that variant.

Once SERIAL was re-enabled to get logs, a new fault path (the one
documented here) opened up: precise data BusFault on uart write, no
VECTTBL, escalates to HardFault because M33 has the BusFault handler
disabled by default in this Zephyr config (no `CONFIG_BUS_FAULT_ISR`
override). LOCKUP message returned.

The two faults look identical at the openocd "clearing lockup after
double fault" level, but CFSR distinguishes them.

## Arch ref §15.4.3.1 confirms the model

Per arch ref §15.4.3.1 "PPC access control rules" (line 12823+):

> NS_ATT — This register indicates Non-secure or Secure for a peripheral region.
> S_P_ATT — This register indicates Secure Privileged/Unprivileged.
> NS_P_ATT — This register indicates Non-Secure Privileged/Unprivileged.

When `cycfg_unused_ppc_cfg.secAttribute = CY_PPC_NON_SECURE` is
applied to a region, NS_ATT[region] is set. ARMv8-M then treats any
Secure-side access via the Secure alias as an attribution mismatch,
which the bus matrix reports as a precise data abort (BusFault).

The NS alias of the same physical peripheral is reachable from a
Secure CPU: ARMv8-M allows Secure to access NS regions via NS aliases
(the converse — NS to Secure — is what TrustZone really blocks).

## Fix options (ranked)

### A. Disable M33 console (applied 2026-04-16)

Set `# CONFIG_SERIAL is not set` (and CONSOLE/PRINTK/BOOT_BANNER) in
`pse84_assistant/sysbuild/enable_cm55.conf`. M33 boots silently. No
boot banner, no `printk` from main(). MBOX/IPC remain available
because the mbox driver uses NS alias `0x422A0000` (per the M33
overlay) which is reachable even when SCB2 is NS-attributed.

**Pros:** zero code change in Zephyr/PDL. Unblocks M33 main() running
+ IPC tunnel.
**Cons:** lose all M33 console output. For BLE/WiFi work, M33 has no
visible logs without a side channel.

### B. Skip SCB2 in `cy_ppc_unsecure_init` (preferred for shared uart)

Add `PROT_PERI0_SCB2` to the skip list in
`zephyr/soc/infineon/edge/pse84/security_config/pse84_s_protection.c`
around line 293-297. SCB2 stays at its default attribute (Secure)
which preserves M33 Secure access via `0x529A0000`.

**Pros:** keeps M33 console working.
**Cons:** breaks M55 console because M55 (NS world) can't access
Secure-attributed SCB2 via NS alias `0x429A0000`. If both cores want
the same uart, this doesn't work.

### C. Move M33 console to a different SCB

Pick an SCB the M55 isn't using (M55 currently grabs SCB2 for
console + PCM dump). M33 uses, say, SCB6 with its own pinctrl. Two
serial bridges (KitProg3 only exposes one — would need a second
USB-CDC adapter or an FTDI cable on the unused SCB pins).

**Pros:** clean separation, both cores can have console.
**Cons:** physical second serial cable required; pinctrl + DT work.

### D. Make the M33 Cy PDL uart driver use NS aliases

In `drivers/serial/uart_infineon_pdl.c`, on Secure builds, mask the
Secure-alias bit in the SCB base address to convert `0x529A0000` →
`0x429A0000`. Or — cleaner — pull `SCB2_BASE` from the NS device
header even on Secure builds (define `MASTER_NSC_AHB5` or whatever the
NS-only macro guard is).

**Pros:** allows shared uart between M33 Secure and M55 NS.
**Cons:** invasive driver change; needs same treatment for every
peripheral M33 uses post-cy_ppc_init.

### E. Don't blanket-attribute PERI0 to NS

Replace the iterate-all-regions loop in `cy_ppc_unsecure_init` with a
selective loop that only flips peripherals M55 needs to NS, leaving
M33-shared ones at their Secure default.

**Pros:** principle of least privilege, no per-peripheral special
cases needed in drivers.
**Cons:** larger PDL change, requires inventory of M55 peripheral
needs.

## Decision for v1 voice-assistant

Going with **Option A** for now to unblock M33 main() running + IPC
tunnel + BLE host bring-up. M33 console silence is acceptable for
v1 (host bridge gets all logs over BLE, debug via openocd/SWO if
needed).

When BLE bring-up needs HCI uart to CYW55513, that will be a
DIFFERENT SCB (uart4 per the existing kit_pse84_eval pinctrl —
verify before committing). Same fault mode would apply if cy_ppc_init
re-attributes that SCB too — then we'd need Option B or D for that
specific SCB.

## How to verify the fix on-chip

1. Reflash with `# CONFIG_SERIAL is not set` in `enable_cm55.conf`.
2. After reset + 3 s, halt M33. PC should be in M33 main() loop
   (0x18100000-0x1810F7C8 range, NOT in `arch_system_halt`).
3. Read SRAM marker at `0x34039f00` — should read `0xBEEF0xxx` if
   M33 main() reached the diagnostic write.
4. CM33 status read should NOT show "clearing lockup after double
   fault".

## Cross-references

- `M33_HARDFAULT_RCA.md` (assistant tree) — original Phase 0a RCA
  (with the wrong root cause; preserved as archaeology)
- `project_pse84_m33_hardfault.md` memory — pre-fault summary, also
  pointed at wrong cause
- arch ref §15.4 — PPC architecture
- `zephyr/soc/infineon/edge/pse84/security_config/pse84_s_protection.c`
  — `cy_ppc_unsecure_init` and `cycfg_unused_ppc_cfg`
- `zephyr/drivers/serial/uart_infineon_pdl.c:324-327` — `ifx_cat1_uart_poll_out`
- `modules/hal/infineon/mtb-template-pse8xxgp/files/devices/include/`
  — `pse846gps2dbzc4a.h` (NS) vs `pse846gps2dbzq3a_s.h` (Secure)
