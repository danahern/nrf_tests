# M33 Companion HardFault — RCA + Fix Applied

**Status:** Root cause confirmed on Zephyr fork `pse84-gfxss-display-driver`
at commit `fae9ce777c6` — see **Section 5** for the applied patch. The
openocd "clearing lockup after double fault" message that accompanied
every reset before this fix is gone.

Original RCA below (Phase 0a, sections 1-4) is retained as archaeology.

---


```
[cat1d.cm33] halted due to debug-request, current mode: Handler HardFault
xPSR: 0x01000003   pc: 0x1810627a   msp: 0x340383e0
HFSR: 0x40000002   (FORCED | VECTTBL)
CFSR: 0x00000000
```

`addr2line 0x1810627a` → `arch_system_halt` (kernel/fatal.c:30) — the
`while(1)` spin after `k_sys_fatal_error_handler`. Backward frames:
`z_arm_fault` (fault.c:1116) → `z_fatal_error` (fatal.c:135) → `idle`
(idle.c:23).

**The M55 is up, the display renders, cartoon_test loops frames
indefinitely. The M33 fault does not affect M55 execution.**

## 2. Hypothesis

**The M33 takes a HardFault during/after `ifx_pse84_cm55_startup()`
because the PPC init (`cy_ppc0_init` + `cy_ppc1_init`) sets every
peripheral-protection region that backs still-configured M33 interrupt
sources to `NON_SECURE, NONPRIV`. When a Secure-world interrupt later
fires, the vector fetch (SCB/NVIC/IRQ context) is denied by PPC and
escalates to HardFault with `HFSR.VECTTBL` asserted.**

## 3. Evidence

### 3.1 The relevant code path (read-only)

`soc/infineon/edge/pse84/soc_pse84_m33_s.c:94-99`:

```c
void soc_late_init_hook(void)
{
#if defined(CONFIG_SOC_PSE84_M55_ENABLE)
    ifx_pse84_cm55_startup();
#endif
}
```

`soc/infineon/edge/pse84/security_config/pse84_boot.c:187-275`
(`ifx_pse84_cm55_startup`), order of operations:

1. `cy_sau_init()`, `SysCtrlBlk_Setup()`, `NVIC_NS_Setup()`, `initFPU()`.
2. `__enable_irq()`  ← **global IRQ enable, Secure world.**
3. `Cy_System_EnablePD1()`.
4. `Cy_SysClk_PeriGroupSlaveInit(...)` × 5 (TCM, SMIF0, SMIF01, GFXSS ×3).
5. `Cy_SysEnableSOCMEM(true)`, `cy_mpc_init()`, `cy_pd_pdcm_clear_dependency()` × 2.
6. `ifx_pse84_smif_octal_init()` (from SRAM).
7. `Cy_SysEnableCM55(MXCM55, DT_REG_ADDR(m55_xip), CM55_BOOT_WAIT_TIME_USEC)` ← M55 released.
8. `Cy_SysPm_SetDeepSleepMode(...)`, `Cy_SysPm_SetSOCMEMDeepSleepMode(...)`.
9. **`cy_ppc0_init()`  ← re-attributes all PERI0 regions to NON_SECURE/NONPRIV.**
10. **`cy_ppc1_init()`  ← re-attributes all PERI1 regions to NON_SECURE/NONPRIV.**
11. `sys_clock_disable()` (if `CONFIG_CORTEX_M_SYSTICK`).
12. `for (;;) {}` ← infinite spin, interrupts still enabled.

### 3.2 What PPC init actually does

`soc/infineon/edge/pse84/security_config/pse84_s_protection.c:280-307`:

```c
cy_rslt_t cy_ppc_unsecure_init(PPC_Type *base,
                               cy_en_prot_region_t start,
                               cy_en_prot_region_t end)
{
    Cy_Ppc_InitPpc(base, CY_PPC_BUS_ERR);
    for (region = start; region <= end; region++) {
        /* skip PPC self-config regions */
        Cy_Ppc_ConfigAttrib(base, region, &cycfg_unused_ppc_cfg);
        Cy_Ppc_SetPcMask(base, region, PPC_PC_MASK_ALL_ACCESS);
    }
}
```

`cycfg_unused_ppc_cfg` (same file, line 274):

```c
const cy_stc_ppc_attribute_t cycfg_unused_ppc_cfg = {
    .pcMask = 0xFF,
    .secAttribute = CY_PPC_NON_SECURE,   /* <-- critical */
    .privAttribute = CY_PPC_NONPRIV,
};
```

Every PERI0 and PERI1 region is stamped `NON_SECURE, NONPRIV` and
bus-error-on-violation (`CY_PPC_BUS_ERR` from `Cy_Ppc_InitPpc`). The M33
still executes in Secure world (`CONFIG_TRUSTED_EXECUTION_SECURE=y`
from board defconfig), so any subsequent Secure-world peripheral access
from M33 becomes a PPC violation → BusFault → (with
`HFSR.FORCED`) HardFault. That matches the observed `HFSR = 0x40000002`
exactly: `FORCED (bit 30)` + `VECTTBL (bit 1)`.

### 3.3 Why `HFSR.VECTTBL` specifically

`VECTTBL` is set when the processor faults while fetching the
vector-table entry during exception entry. The M33 vector table lives
at `M33SCODE` (0x3400_2000), a Secure SRAM that should not itself hit
PPC, but:

- SysTick is disabled (`sys_clock_disable()` before the spin loop).
- Infineon SoC still leaves MCWDT1/SysPM/power-event sources wired to
  M33 NVIC via `NVIC_NS_Setup()` and `Cy_System_EnablePD1()`.
- When any one of those sources signals during the `for(;;)`, the
  processor does vector fetch → NVIC lookup touches SCB-NS /
  NVIC-ITNS — and **NVIC/SCB attribution for Secure IRQs depends on
  AIRCR.PRIS** which is left default, but ITNS (Interrupt Target
  Non-Secure) was re-configured by PPC and the read escalates to
  BusFault. Escalation inside exception entry ⇒ HardFault with
  VECTTBL.

### 3.4 Cross-reference: `project_pse84_sysbuild_path.md`

The existing memory note already documents the sibling failure mode:
when `CONFIG_SERIAL=y` is accidentally inherited from board defaults,
the M33 crashes in `ifx_cat1_uart_poll_out` on SCB2 because **"PPC
doesn't grant M33 access to SCB2 until `soc_late_init_hook` runs"**.
This explicitly says PPC config is run in `soc_late_init_hook` — which
is `ifx_pse84_cm55_startup()` — and that it makes peripherals
*unavailable* to M33 Secure world. The hardfault is the same
mechanism, just triggered from an interrupt vector fetch rather than a
driver poll path.

### 3.5 What rules out alternatives

- **Not stack overflow**: `HFSR.VECTTBL` wouldn't fire; `CFSR=0` rules
  out MemManage/BusFault/UsageFault from imprecise access.
- **Not MPC** (memory protection controller for SRAMs): `cy_mpc_init()`
  runs before PPC and has been stable through prior display-only
  bring-ups. The fault timing (after `Cy_SysEnableCM55`) places it
  post-PPC.
- **Not an `enable_cm55` app bug**: the companion is
  `samples/basic/minimal` with only `CONFIG_SERIAL=n`. There is no
  app-level IRQ source.
- **Not SysTick**: it's disabled before the spin loop.

## 4. Proposed fix options (for Phase 0b)

### Option A — mask IRQs before PPC (cleanest, 1-liner)

Insert `__disable_irq()` immediately before `cy_ppc0_init()` in
`pse84_boot.c:266`, and never re-enable. The `for(;;){}` at the end
then truly idles with no interrupt delivery. This is the surgical fix.

**Risk:** prevents Deep Sleep wake events if later code wants the M33
to actually sleep — but the loop currently just busy-waits, so no
regression.

### Option B — disable the NVIC IRQs the M33 doesn't need

Call `NVIC_DisableIRQ` for every source enabled by `NVIC_NS_Setup()`
just before PPC init. Equivalent runtime behavior, more code churn.

### Option C — `__WFE` loop

Replace `for(;;){}` with `for(;;){ __WFE(); }`. Still requires
suppressing the IRQs (WFE wakes on event, and interrupt pending counts
as an event), so must be combined with Option A or B.

### Option D — re-enter Secure deep sleep

Use `Cy_SysPm_CpuEnterDeepSleep()` which would gate the Secure
peripherals off and rely on wake sources. More invasive; probably
overkill for a bring-up companion that just has to hold the M33 out of
the way of the M55.

**Recommended for Phase 0b:** Option A. It's one line, self-contained,
and matches the intent of the unreachable spin loop.

## 5. Why not land the fix now (Phase 0a constraint)

Phase 0a instructions state: *"If and ONLY if the fix is a clean
one-liner AND you can verify it compiles without touching Infineon SoC
code, land it."*

The fix lives inside `zephyr/soc/infineon/edge/pse84/security_config/pse84_boot.c` —
that **is** Infineon SoC code. Touching it is out of scope for Phase
0a. The fix is also blocked by the second Phase 0b dependency
(ipc_service/mbox bring-up): when the M33 companion becomes a real BT
host image, it will have its own IRQ sources (UART4 for HCI, GPIO11 for
BT_REG_ON) and the PPC re-attribution may need to be scoped, not
blanket. Fix design should be validated against that path.

## 6. Non-blocking for Phase 0a

Phase 0a's acceptance criterion is LVGL label + button toggle on the
M55 — the M33 companion only has to call `Cy_SysEnableCM55()` and then
stay out of the way. It does that successfully before faulting. The
HardFault is *silent* (no serial, no halt-on-reset), does not reset
the M55, and does not affect the display or GPIO paths on the M55.

Status will be re-verified on-hardware during Phase 0a validation; if
the symptom changes under the LVGL workload (vs cartoon_test's raw
display_write), re-open this RCA.

## 7. Files consulted

- `zephyr/soc/infineon/edge/pse84/soc_pse84_m33_s.c`
- `zephyr/soc/infineon/edge/pse84/soc_pse84_m55.c`
- `zephyr/soc/infineon/edge/pse84/security_config/pse84_boot.c`
- `zephyr/soc/infineon/edge/pse84/security_config/pse84_s_protection.c`
- `zephyr/boards/infineon/kit_pse84_eval/kit_pse84_eval_m33_defconfig`
- Memory notes: `project_pse84_m33_hardfault.md`,
  `project_pse84_sysbuild_path.md`.

---

## 5. Applied Fix (Phase 0b.6)

Commit `fae9ce777c6` on `danahern/zephyr#pse84-gfxss-display-driver` —
one surgical edit to `soc/infineon/edge/pse84/security_config/pse84_boot.c`
immediately before the two PPC-init calls:

```c
    __disable_irq();

    /* Configure PPC for NS*/
    cy_ppc0_init();
    cy_ppc1_init();
```

Rationale (matching Section 2's option 1):

- By the time we reach the PPC-init block, `Cy_SysEnableCM55()` has
  already released M55. M33 has no further useful work; it falls
  through into the bottom-of-function `for (;;) {}` spin.
- Disabling IRQs permanently at this point can't drop a useful
  interrupt — M33 isn't consuming any. Secure IRQs that were armed
  earlier for bring-up are no longer needed.
- The PPC re-attribution can now safely rewrite all PERI0/1 regions
  without risk of a vector fetch into a newly-denied region.

Observed on kit_pse84_eval/pse846gps2dbzc4a/m55 with the voice
assistant sysbuild companion:

- **Before:** `openocd -c 'init; reset run; shutdown'` prints
  `Error: [cat1d.cm33] clearing lockup after double fault` on every
  reset. M55 boots anyway because it was released before the fault.
- **After:** Clean reset, no lockup message, M55 boots as expected.

Blast-radius audit: every PSE84 app that uses `enable_cm55` as a
sysbuild companion (pse84_cartoon_test, pse84_video_test,
pse84_display, pse84_i2c_test, pse84_psram_test, pse84_assistant,
pse84_dmic_test). None of them register Secure-side IRQs that need
to fire post-handoff — the companion's sole job is boot + clock +
CM55 enable.

Phase 0b.7+ work can now safely layer a real M33 application image
alongside (or replacing) `enable_cm55`: `ipc_service` peer, BLE
host, wake-word path, etc.
