# Phase 4 Scouting: M33 BT Stack Bring-Up on kit_pse84_eval_m33

Read-only feasibility pass for bringing up Zephyr Bluetooth host + L2CAP CoC
on the on-board CYW55513 over HCI UART, using **only** the Infineon Zephyr
tree under `zephyr_workspace/zephyrproject/`.

## 1. Verified sample on this board?

**No.** No BT sample or test in-tree targets `kit_pse84_eval_m33`.
Searched `zephyr/samples/bluetooth/**` and `zephyr/tests/bluetooth/**`:
only PWM / counter / blinky overlays reference the board. The
`infineon,bt-hci-uart` driver is used by CYW4343W/4373 combo boards
(Arduino Portenta/Nicla/Opta/Giga, `cy8cproto_062_4343w`); CYW55513 is
newer and has **no** known-good sample.

Closest reference: `zephyr/samples/bluetooth/peripheral/prj.conf`
combined with `l2cap_coc_initiator/prj.conf` — Phase 4 will synthesise
from these.

## 2. `hci_uart_infineon.c` end-to-end

`zephyr/drivers/bluetooth/hci/hci_uart_infineon.c` (385 LOC). Flow:

1. `gpio_pin_set_dt(bt_reg_on, 1)` after 300 ms CBUCK discharge (L:282).
2. 500 ms settling (L:299).
3. **If `CONFIG_AIROC_AUTOBAUD_MODE`** and `fw-download-speed != current-speed`,
   switch host UART baud first (L:305-311). Required for CYW555xx — in
   Download Mode the VSC `UPDATE_BAUDRATE` is rejected until the
   minidriver is up.
4. `HCI_RESET` → (else-branch) VSC `UPDATE_BAUDRATE` (L:314-326).
5. `bt_firmware_download(brcm_patchram_buf, brcm_patch_ram_length)` — walks
   the `.hcd` blob, issues `WRITE_RAM`/`LAUNCH_RAM` (L:179-236).
6. 250 ms stabilisation, restore default baud, second `HCI_RESET`,
   optional `hci-operation-speed` re-baud, optional `SET_MAC`.
7. **`CONFIG_BT_CYW555XX` branch (L:370-383)**: sends VSC
   `WRITE_PCM_INT_PARAM` to move SCO route to PCM — required so
   `Host_Buffer_Size` doesn't get rejected. Auto-enabled by
   `CYW55513IUBG_SM` via `Kconfig.infineon:258`.

No TODOs, no `#warning`. Clean driver.

## 3. BT firmware blob discovery

`zephyr/modules/hal_infineon/btstack-integration/CMakeLists.txt:102`:

```
if(CONFIG_CYW55513IUBG_SM)
  set(blob_hcd_file ${hal_blobs_dir}/COMPONENT_CYW55513/COMPONENT_BTFW/bt_firmware.hcd)
endif()
...
generate_inc_file_for_target(app ${blob_hcd_file} ${blob_gen_inc_file})
```

The `.hcd` is byte-included into `brcm_patchram_buf[]` in
`w_bt_firmware_controller.c` via `#include <bt_firmware.hcd.inc>`.

Blob source is declared in `modules/hal/infineon/zephyr/module.yml:742`
pointing at `bt-fw-ifx-cyw55500a1/raw/release-v2.2.0/COMPONENT_wlbga_iPA_sLNA_ANT0_LHL_XTAL_IN/btfw.hcd`
(SM module = single-antenna iPA/sLNA variant, not BTANT).

**Action required**: `west blobs fetch hal_infineon` — currently only
`modules/hal/infineon/zephyr/blobs/license.txt` exists. The separate
`mtb_shared/bt-fw-ifx-cyw55500a1/` tree is from ModusToolbox and is
**not** referenced by the Zephyr CMake; ignore it.

## 4. Proposed Phase 4 starting point

Application dir: `zephyr_workspace/pse84_assistant/` (owned by Phase 0
agent — propose a `phase4_ble_hello/` subfolder once Phase 0 lands).

`prj.conf` (minimal advertising peripheral + dynamic L2CAP PSM):

```
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_DEVICE_NAME="pse84-voice"
CONFIG_BT_L2CAP_DYNAMIC_CHANNEL=y
CONFIG_BT_BUF_ACL_RX_SIZE=255
CONFIG_BT_BUF_ACL_TX_SIZE=251
CONFIG_BT_L2CAP_TX_MTU=247
# HCI UART / AIROC
CONFIG_BT_HCI_SETUP=y
CONFIG_AIROC_AUTOBAUD_MODE=y
# CYW55513IUBG_SM + BT_CYW555XX auto-selected by board Kconfig.defconfig
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_GPIO=y
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_HEAP_MEM_POOL_SIZE=8192
```

Overlay: **none needed** — `kit_pse84_eval_m33.dts:218-229` already
instantiates the `zephyr,bt-hci-uart` + `infineon,bt-hci-uart` child
with `bt-reg-on-gpios = <&gpio_prt11 0>`, `fw-download-speed =
<115200>`, `hci-operation-speed = <115200>`, `hw-flow-control`. Bump
both speeds (e.g. 3000000) in an overlay later for throughput.

`main.c` skeleton: call `bt_enable(NULL)`, register one
`bt_l2cap_server` at PSM 0x0080 (dynamic range starts 0x0080), start
`BT_LE_ADV_CONN_FAST_1`, log on `chan->connected`.

## 5. Risks / gotchas

- **Blob not fetched**: build will succeed with an empty
  `brcm_patchram_buf[]` and silently no-op the FW download, then the
  second `HCI_RESET` will time out. Must run `west blobs fetch
  hal_infineon` first and verify
  `modules/hal/infineon/zephyr/blobs/img/bluetooth/firmware/COMPONENT_CYW55513/COMPONENT_BTFW/bt_firmware.hcd`
  exists.
- **`CONFIG_AIROC_AUTOBAUD_MODE` is mandatory** for CYW555xx when
  `fw-download-speed != current-speed` (driver comment L:301-304). Safe
  to leave on even at 115200/115200 — block is guarded by speed check.
- **`hw-flow-control` is a BUILD_ASSERT** (`hci_uart_infineon.c:33`).
  Don't override the DTS to disable RTS/CTS.
- `CONFIG_BT_BUF_CMD_TX_SIZE` is force-defaulted to 255 by
  `Kconfig.infineon:288` — don't override; the FW download uses
  oversize VSC packets.
- `BT_MAIN_STACK_SIZE` default 2048 from board defconfig — keep >=2048.
- **TrustZone**: board M33 defconfig enables
  `CONFIG_TRUSTED_EXECUTION_SECURE=y`; HCI UART + GPIO11.0 must be in
  Secure world. If Phase 4 moves to `_ns`, add TF-M partition for
  SCB4 + GPIO11.
- FW version `CYW55500A1_001.002.032.0145` (mtb_shared `fw_version.txt`).
  BT 5.4 LE, PAwR/ISO not guaranteed — stick to classic LE + L2CAP CoC.

## 6. Recommendation: **YELLOW**

Justification: every glue piece is present in-tree — driver, Kconfig
auto-select (`CYW55513IUBG_SM` → `BT_CYW555XX`), board DTS node, blob
manifest. But **nothing exercises this path yet on pse84**, blobs must
be fetched, and the Autobaud + SCO-route fixups imply Infineon expects
first-boot issues. Budget ~1–2 days for first successful
`advertising_start` (most likely failure modes: blob missing, RTS/CTS
pin mux, TF-M secure attribution of GPIO11). No missing source =
not red; no verified sample = not green.

### Top 3 risks
1. `west blobs fetch hal_infineon` not run → silent patchram bypass → HCI timeout.
2. TF-M secure-world attribution of SCB4/GPIO11.0 when moving to `_m33_ns`.
3. CYW55513 A1 FW quirks at non-default baud (3 Mbps) — stay at 115200 for PoC.
