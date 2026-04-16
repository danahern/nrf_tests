# PSE84 Voice Assistant — Context-Reload Reference

**Purpose:** When a session starts cold (compaction, new chat), this doc primes
the next agent on everything load-bearing about the kit, the boot pipeline, and
the gotchas that take hours to rediscover. Every fact here was paid for in
debug time. Keep it current — when something new lands, append.

> **Companion docs:**
> - `docs/pse84_m33_rram_investigation.md` — full debug trail (chronological)
> - `docs/pse84_octal_policy_enablement.md` — extended-boot policy provisioning
> - `docs/pse84_psram_handoff.md` — SMIF1 PSRAM bring-up
> - `docs/infineon/text/pse_e8x_architecture_reference_manual.txt` — RAG'd ARM ref
> - Memory: `feedback_*` and `project_pse84_*` in
>   `~/.claude/projects/-Users-danahern-code-claude-embedded/memory/`

---

## 1. Two working trees, one Zephyr

```
/Users/danahern/code/claude/embedded/                # primary working dir (this CLAUDE.md)
/Users/danahern/code/claude/embedded-assistant/      # PSE84 voice assistant branch
```

`embedded-assistant/zephyr_workspace/zephyrproject` is a **symlink** to
`embedded/zephyr_workspace/zephyrproject`. Edits to `zephyr/soc/...`,
`zephyr/drivers/...` etc. apply to BOTH. Edits to
`embedded-assistant/zephyr_workspace/pse84_assistant*` only apply there.

The voice-assistant work lives on branch `pse84-voice-assistant` of the
`embedded-assistant` repo, and on branch `pse84-gfxss-display-driver` of
the inner `zephyrproject/zephyr` submodule.

---

## 2. The boot pipeline (must be understood end-to-end)

```
Power-on
   │
   ▼
SROM Boot v2.0.0.6022           — fixed in chip ROM
   │
   ▼
RRAM Boot v2.0.0.7127           — fixed in RRAM
   │
   ▼
SE RT Services v1.0.0.2361      — fixed
   │
   ▼
Extended Boot v1.1.0.1700       — reads OEM policy from SE config
   │ (configures SMIF0 per policy.external_flash.smif_chip_select +
   │  smif_data_width; determines next-app source)
   │
   ▼
LISTEN_WINDOW (0xAA00B5F8)      — gives debugger time to attach
   │ (timeout = policy.listen_window ms; defaults 100)
   │
   ▼
oem_app_address (S-AHB SMIF0 Secure alias 0x70100000 in our policy)
   │
   ▼
Our M33 image (header @ 0x60100000 NS / 0x70100000 S, code @ 0x18100400 C-AHB)
   │
   ▼
Zephyr boot: __reset → soc_early_init_hook
   │  (in our fork: ifx_pse84_cm55_early_init runs here —
   │   SAU/MPC/PPC attribution + PD1 + peri groups + SOCMEM)
   │
   ▼
Zephyr driver probes (PRE_KERNEL_1 → POST_KERNEL)
   │  (uart2/SCB2, mbox/pse84_mbox now safe to probe)
   │
   ▼
soc_late_init_hook → ifx_pse84_cm55_startup
   │  (PSRAM init + Cy_SysEnableCM55)
   │
   ▼
M33 main() runs                 — heartbeats, IPC, log relay
M55 reset vector                — display, audio, Phase 4 BLE
```

---

## 3. The OEM policy (extended-boot config)

Source of truth: `zephyr_workspace/pse84_provision/policy/policy_oem_octal.json`

Critical fields (current working values):
| Field | Value | Why |
|---|---|---|
| `oem_app_address` | `0x70100000` | SMIF0 Secure S-AHB. (NOT 0x18100000 C-AHB — extended-boot rejects with `IFX_ERR_UNKNOWN_MEMORY_RANGE`) |
| `oem_alt_boot` | `true` | Required! `false` causes M33 cy_mpc_init to fault on locked APPCPUSS rows. (`true` leaves attribution layout Zephyr expects.) |
| `oem_alt_app_address` | `0x70100000` | Set same as primary so either path lands at our M33. |
| `smif_chip_select` | `0` | CS0 = octal NOR S28HS01GT. |
| `smif_data_width` | `8` | Octal mode. |
| `enable_alt_serial` | `true` | Required for alt_boot path. |
| `listen_window` | `100` | ms. |

### Reprovisioning command

DEV LCS (our chip) MUST use `provision-device` not `reprovision-device`
(reprovision is PROD-LCS only):

```bash
cd /Users/danahern/code/claude/embedded/zephyr_workspace/pse84_provision
edgeprotecttools -t pse8xs2 provision-device \
  -p policy/policy_oem_octal.json \
  --key keys/oem_rot_priv_0.pem \
  --ifx-oem-cert packets/oem_cert_signed.bin
```

The `keys/`, `packets/`, `.edgeprotecttools` assets live ONLY in the
`/Users/danahern/code/claude/embedded/` tree — not in the assistant
worktree.

If the policy needs an `extended_boot_image` block stripped before
running (depends on path), back it up first.

---

## 4. Memory map cheat-sheet (arch ref §2.2.6)

| Range | Bus | Region | Size |
|---|---|---|---|
| `0x02000000-0x0207FFFF` | C-AHB NS | RRAM | 512 KB |
| `0x12000000-0x1207FFFF` | C-AHB Secure | RRAM | 512 KB |
| `0x18000000-0x1FFFFFFF` | C-AHB Secure | SMIF0 XIP | up to 64 MB |
| `0x22000000-0x2207FFFF` | S-AHB NS | RRAM | 512 KB |
| `0x32000000-0x3207FFFF` | S-AHB Secure | RRAM | 512 KB |
| `0x60000000-0x63FFFFFF` | S-AHB NS | SMIF0 XIP | up to 64 MB |
| `0x64000000-0x647FFFFF` | S-AHB NS | SMIF1 PSRAM | 16 MB |
| `0x70000000-0x73FFFFFF` | S-AHB Secure | SMIF0 XIP | up to 64 MB |
| `0x34000000-0x340FFFFF` | (any) | SRAM (SE workspace + boot status) | 1 MB |

**M33 code** lives at:
- Header @ `0x60100000` (NS S-AHB) = `0x70100000` (Secure S-AHB) — same physical
- Code (vector table + .text) executes from `0x18100400` (Secure C-AHB)

**M55 code** lives at:
- Image @ `0x60500000` (NS S-AHB) = `0x70500000` (Secure S-AHB)
- Code executes from `0x18500000` (Secure C-AHB) for vectors, `0x60500400` for code

`oem_app_address` MUST be in S-AHB range (`0x32xxxxxx` or `0x70xxxxxx`).
Extended-boot rejects C-AHB addresses with `IFX_ERR_UNKNOWN_MEMORY_RANGE`.

---

## 5. Extended-boot status codes (arch ref §17.2.4.2.5)

Status word at SRAM `0x34000000`:

| Code | Meaning |
|---|---|
| `0xAA000000` | `IFX_BOOT_MODE_MAIN` — main flow |
| `0xAA000001` | `IFX_BOOT_MODE_RECOVERY` |
| `0xAA000002` | `IFX_BOOT_MODE_DFU` — alt-serial |
| `0xAA000003` | `IFX_BOOT_MODE_LAUNCH_NEXT_APP` — control passed |
| `0xAA000004` | `IFX_BOOT_MODE_STARTUP` |
| `0xAA00B5F8` | `IFX_BOOT_MODE_LISTEN_WINDOW` — debugger window |

Error word at SRAM `0x34000004` (when status is 0xEEx_xxxx):

| Code | Meaning |
|---|---|
| `0xEE000000` | `IFX_ERR_NO_ERRORS` |
| `0xEE000024` | `IFX_ERR_UNKNOWN_MEMORY_RANGE` (oem_app_address bad) |
| `0xEE00000F` | `IFX_ERR_VALIDATION_WRONG_IMG` |
| `0xEE000010` | `IFX_ERR_SEC_BOOT_VALIDATION_FAILED` |
| `0xEE000011` | `IFX_ERR_VALIDATION_IMG_VECT_T*` |

LCS at SRAM `0x52400420..0x5240042c`:
- `0x000000d780000b76bc0000db 00000000` = **DEVELOPMENT LCS**

---

## 6. THE LISTEN_WINDOW GOTCHA (load-bearing — discovered 2026-04-16)

**Symptom:** Halt M33 → PC = `0x12004f82`, instruction = `0xe7fe` (`B .` —
branch to self). Looks like a fault loop. **It's not.**

**Cause:** Extended-boot enters listen window (`0xAA00B5F8`) when it detects
debugger acquisition. The instruction at PC is the listen-window spin loop —
extended-boot voluntarily idles to give SWD time to attach.

**Fix for inspection:** Don't trust state when openocd is connected. To
verify whether M33 is actually running, must observe via a side channel
(serial output, MPC writes visible to M55, etc.) WITHOUT openocd attached.

**Implication:** The line:
```
xPSR: 0x29000000 pc: 0x12004f82 msp: 0x34005a48
```
returned by `openocd halt; reg pc` does NOT mean M33 hung. It means
"openocd parked extended-boot in listen window during acquire."

---

## 7. Flashing pipeline

### Build (sysbuild — both M33 + M55 in one tree)

```bash
cd /Users/danahern/code/claude/embedded-assistant/zephyr_workspace/zephyrproject
west build -b kit_pse84_eval/pse846gps2dbzc4a/m55 \
  ../pse84_assistant -p auto \
  --sysbuild -d /tmp/build_assistant
```

Outputs:
- M55 image: `/tmp/build_assistant/pse84_assistant/zephyr/zephyr.hex` (LMA `0x60500000+`)
- M33 image: `/tmp/build_assistant/enable_cm55/zephyr/zephyr.signed.hex` (LMA `0x60100000+`)

### Flash both images via openocd (octal 64 MB SMIF0)

```bash
SMIF_OCTAL=1 \
SMIF_OCTAL_FLM=/Users/danahern/code/claude/embedded-assistant/zephyr_workspace/pse84_octal_enablement/cycfg_octal_generated/PSE84_SMIF.FLM \
/Applications/ModusToolboxProgtools-1.7/openocd/bin/openocd \
  -s /Users/danahern/code/claude/embedded-assistant/zephyr_workspace/zephyrproject/zephyr/boards/infineon/kit_pse84_eval/support \
  -s /Applications/ModusToolboxProgtools-1.7/openocd/scripts \
  -f /Users/danahern/code/claude/embedded-assistant/zephyr_workspace/zephyrproject/zephyr/boards/infineon/kit_pse84_eval/support/openocd.cfg \
  -c 'init' \
  -c 'targets cat1d.cm33' \
  -c 'reset halt' \
  -c 'flash banks' \
  -c 'flash write_image erase /tmp/build_assistant/pse84_assistant/zephyr/zephyr.hex' \
  -c 'flash write_image erase /tmp/build_assistant/enable_cm55/zephyr/zephyr.signed.hex' \
  -c 'reset run' -c 'shutdown'
```

**Without `SMIF_OCTAL=1`** the flash bank silently caps at 16 MB (Quad CS1
slot) — anything past `0x61000000` gets DROPPED with a success status.
Symptom: chip boots stale image.

**Without `SMIF_OCTAL_FLM`** pointing at the patched FLM (CS0 octal),
writes either fail or write to CS1 (which we don't use). The patched FLM
is generated via MTB qspi-configurator-cli.

### M55-only quick reflash

After M33 is correct, M55 changes only need re-writing the M55 hex.

---

## 8. Driver init level constraint (load-bearing)

Zephyr's `DEVICE_DT_INST_DEFINE` macro chain (in `include/zephyr/init.h`)
**rejects `APPLICATION` level for device-class drivers** at compile time
(`ZERO_OR_COMPILE_ERROR` macro). So you can NOT just bump `mbox_pse84` or
`uart_infineon_pdl` to `APPLICATION` to defer their probes.

**This means:** if a driver touches a peripheral that needs PPC re-attribution
to NS first, the attribution must happen **earlier than driver probe**, not
later. Hence the SOC hook split (item 9).

---

## 9. SOC hook split (load-bearing — 2026-04-16)

**Problem:** With `CONFIG_SOC_PSE84_M55_ENABLE=y`, all PPC re-attribution
used to live in `ifx_pse84_cm55_startup()` called from `soc_late_init_hook`
(after POST_KERNEL). But uart2 / mbox driver probes at PRE_KERNEL_1 and
POST_KERNEL touch SCB2 / pse84_mbox, which are Secure-by-default until PPC
re-attributes them — bus-fault on first probe.

**Fix:** Split into two phases (in `zephyr/soc/infineon/edge/pse84/`):

`security_config/pse84_boot.c`:
- New `ifx_pse84_cm55_early_init()` — runs from `soc_early_init_hook`,
  BEFORE driver probes. Does SAU + SCB/NVIC/FPU + PD1 + peri groups +
  SOCMEM + `cy_mpc_init` + `cy_pd_pdcm_clear_dependency` + `cy_ppc0_init`
  + `cy_ppc1_init`. (No `__disable_irq` dance — M55 isn't released yet,
  so no concurrent IRQ traffic to worry about.)
- `ifx_pse84_cm55_startup()` — late phase. Now ONLY does
  `ifx_pse84_psram_init` + `Cy_SysEnableCM55` + `Cy_SysPm_SetSOCMEMDeepSleepMode`.

`security_config/pse84_boot.h`:
- Added `void ifx_pse84_cm55_early_init(void);`

`soc_pse84_m33_s.c`:
- `soc_early_init_hook` calls `ifx_pse84_cm55_early_init()` after `SystemInit()`.
- `soc_late_init_hook` retains `ifx_pse84_cm55_startup()` call.

**Trade-off:** Zephyr boot-banner ("\*\*\* Booting Zephyr OS build … \*\*\*")
prints during PRE_KERNEL_2 via `cbprintf`. uart driver IS probed and live by
then (PRE_KERNEL_1 default), so the banner does emit. Confirmed working
in pse84_assistant_m33 once we verify on-chip.

---

## 10. Common pitfalls — quick reference

| Symptom | Cause | Fix |
|---|---|---|
| M33 PC stuck at `0x12004f82` | Listen window for debugger | Disconnect openocd, observe via serial |
| CM55 stuck at `0x3ff00` | `Cy_SysEnableCM55` never called | Set `CONFIG_SOC_PSE84_M55_ENABLE=y` in **enable_cm55.conf** explicitly (sysbuild only forces it for samples/basic/minimal fallback) |
| openocd "wrote N bytes" but stale image runs | Flash bank capped at 16 MB without `SMIF_OCTAL=1` | Set both `SMIF_OCTAL=1` AND `SMIF_OCTAL_FLM=/path/to/patched.FLM` |
| `extended-boot rejects oem_app_address` (`IFX_ERR_UNKNOWN_MEMORY_RANGE`) | Address in C-AHB range (`0x18xxxxxx`) | Use S-AHB alias (`0x70xxxxxx` Secure or `0x60xxxxxx` NS) |
| M33 cy_mpc_init faults on APPCPUSS row | `oem_alt_boot: false` locks rows | Set `oem_alt_boot: true` and reprovision |
| M33 silent past Zephyr boot banner | uart driver bus-faults pre-attribution | SOC hook split (item 9) |
| display lit but not animating | M55 stuck in UART IRQ storm or LOG deferred-thread starvation | `CONFIG_UART_INTERRUPT_DRIVEN=n` + `CONFIG_LOG_MODE_IMMEDIATE=y` on M55 |
| 8 minute build | `CONFIG_APP_ANIMATION_SPRITES=y` (71 MB asset bundle) | Set =n; iterates in 12s |
| `reprovision-device` rejects with "PRODUCTION LCS" | We're in DEV LCS | Use `provision-device` not `reprovision-device` |
| `reprovision-device` rejects with "extended_boot_image not allowed" | Property block in policy | Strip `extended_boot_image` from policy file |

---

## 11. Halt + inspect cheatsheet (when you DO want to debug)

Read M33 PC + boot status:
```bash
SMIF_OCTAL=1 SMIF_OCTAL_FLM=/path/to/patched.FLM \
/Applications/ModusToolboxProgtools-1.7/openocd/bin/openocd \
  -s /path/to/support -s /Applications/ModusToolboxProgtools-1.7/openocd/scripts \
  -f /path/to/support/openocd.cfg \
  -c 'init' -c 'targets cat1d.cm33' -c 'halt' \
  -c 'reg pc' -c 'reg sp' -c 'reg lr' \
  -c 'mdw 0x34000000 4'  # status + error words \
  -c 'mdw 0xE000ED28 4'  # CFSR \
  -c 'mdw 0xE000ED2C 4'  # HFSR \
  -c 'targets cat1d.cm55' -c 'halt' \
  -c 'reg pc' \
  -c 'shutdown'
```

Read flashed bytes at SMIF0:
```bash
... -c 'mdw 0x60100000 16'  # M33 header (look for magic 0x96f3b83d)
... -c 'mdw 0x60100400 16'  # M33 vector table (SP, Reset_Handler)
... -c 'mdw 0x60500000 16'  # M55 vector table
```

Decode boot status code with `docs/infineon/text/pse_e8x_architecture_reference_manual.txt`
§17.2.4.2.5 (line ~18427).

---

## 12. Serial capture (kit_pse84_eval)

UART2 = KitProg3 USB CDC = `/dev/tty.usbmodem1103` @ 115200 baud (M33
default), or 460800 if M55 overlay overrides (current pse84_assistant
M55 overlay does this for PCM dump).

```python
import serial, time, sys
s = serial.Serial('/dev/tty.usbmodem1103', 115200, timeout=0.1)
end = time.time() + 15
while time.time() < end:
    data = s.read(4096)
    if data: sys.stdout.write(data.decode('utf-8', errors='replace')); sys.stdout.flush()
s.close()
```

Use `~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3` (project
CLAUDE.md mandates it for PyObjC tooling).

KitProg3 does NOT respond to DTR-toggle reset. To reset, fire openocd
with `-c 'init' -c 'reset run' -c 'shutdown'` then open serial.

---

## 13. Phase 4 BLE — DONE (2026-04-16)

**BLE on M55, not M33.** The original master plan put BLE host on M33,
but the SCB4 (uart4 = HCI to CYW55513) faults the same way SCB2 does
when accessed from M33-Secure post-`cy_ppc_init`. The cleanest unblock
was to put BLE on M55 NS (where there's no PPC issue).

`kit_pse84_ai_m55.dts` (the AI variant) already shows the pattern —
same hardware (CYW55513, uart4 HCI, GPIO11.0 reg-on), BT on M55. We
mirror that in `pse84_assistant`'s board overlay (already present at
lines 340-360 of the M55 overlay) plus add the M55 prj.conf knobs:

```
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_DEVICE_NAME="PSE84-Assistant"
CONFIG_BT_L2CAP_DYNAMIC_CHANNEL=y
CONFIG_BT_HCI_ACL_FLOW_CONTROL=n
CONFIG_BT_AIROC=y
CONFIG_CYW55500=y
CONFIG_CYW55513IUBG_SM=y
```

BT firmware blob is fetched at:
```
modules/hal/infineon/zephyr/blobs/img/bluetooth/firmware/COMPONENT_CYW55513/COMPONENT_BTFW/bt_firmware.hcd
```

Firmware downloads over HCI uart at 115200 → ~18 s startup. Bumping
`fw-download-speed` in DT (cyw55513 node) to 3000000 should drop this
to <2 s; not yet tried.

**Verified output (post-fw-load):**
```
bt_hci_core: HCI transport: H:4
bt_hci_core: Identity: 55:50:0A:1A:76:93 (public)
bt_hci_core: HCI: version 6.0 (0x0e) revision 0x1000, manufacturer 0x0009
bt_hci_core: LMP: version 6.0 (0x0e) subver 0x2220
ble: bt_enable ok; starting advertising (name='PSE84-Assistant')
ble: advertising as 'PSE84-Assistant'
```

**Pitfall observed:** `ble_init()` was called twice in `main.c` (once
before the IPC block, once after). Second call re-runs `k_thread_create`
on a non-idle thread → `bt_enable`'s internal timer setup hits
`__ASSERT(!sys_dnode_is_linked(&to->node))` in `kernel/timeout.c:100`.
Fix: call `ble_init()` once.

**Phase 4 still TODO:**
- L2CAP CoC handler (LE_PSM 0x0080, 247 B MTU per master plan)
- `|type|seq|len|payload|` framing matching `bridge.py`
- Audio path: PDM → Opus → L2CAP. With BLE on M55, all of this
  runs on the same core. CPU budget verification needed —
  current dmic_read errors with FIFO overflow suggest BT thread
  starves audio. Likely fixes:
  - Bump audio thread priority above ble_thread (currently
    ble at K_PRIO_PREEMPT(7))
  - Defer Opus encode (do it on demand via DMIC ring buffer)

**M33 still silent:** companion just calls `ifx_pse84_cm55_startup`
(boots M55 + does PSRAM/octal SMIF config) and idles. No IPC tunnel
yet. With BLE on M55, IPC isn't blocking for v1.

## 14. WiFi (stretch — same chip)

CYW55513 is dual-mode (BLE + WiFi). WiFi runs over SDHC0 (SDIO).
Board DT already has the `airoc-wifi` node:
```
sdhc0 {
    airoc-wifi {
        compatible = "infineon,airoc-wifi";
        wifi-reg-on-gpios = <&gpio_prt11 6 GPIO_ACTIVE_HIGH>;
        wifi-host-wake-gpios = <&gpio_prt11 4 GPIO_ACTIVE_HIGH>;
    };
};
```

Need:
- `CONFIG_WIFI=y CONFIG_WIFI_AIROC=y CONFIG_AIROC_WIFI_CYW55513=y`
- WHD firmware blob (already fetched: `img/whd/resources/firmware/COMPONENT_55500/COMPONENT_SM/55500A1.trxcse`)
- CLM blob (`img/whd/resources/clm/COMPONENT_55500/COMPONENT_CYW55513IUBG/55500A1.clm_blob`)
- Network config (NETWORKING=y, NET_IPV4=y, etc.)
