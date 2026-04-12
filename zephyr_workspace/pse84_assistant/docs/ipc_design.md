# PSE84 M33 ↔ M55 IPC Design (Phase 0b)

**Status:** Proposal. All open questions marked with `Q:` are deferred
to the Phase 0b review gate. Implementation belongs to a separate
metaswarm cycle; nothing here builds yet.

## 1. Goal

Move typed binary messages between two Zephyr images on the same
PSE84: the **M33** (Secure, CYW55513 BT host — Phase 4) and the **M55**
(Non-Secure, app core: audio, LVGL, state machine). Framing is the
master-plan L2CAP CoC header:

```
| u8 type | u8 seq | u16 len | payload[len] |
```

The M33 is a dumb forwarder: whatever comes off L2CAP goes through IPC
verbatim to the M55 and back. Framing is interpreted only on the host
and on the M55 — never on the M33.

## 2. Backend choice: `icmsg` (not OpenAMP / RPMsg)

### 2.1 Why icmsg

- **Single endpoint, fixed peers** — there is exactly one M33 image and
  one M55 image; no runtime discovery needed.
- **Smaller footprint** — the whole icmsg send path is ~150 LOC + SPSC
  ring buffer. OpenAMP drags VirtIO + resource table + shared address
  space bring-up — overkill for one endpoint.
- **Header-only ring discipline** — icmsg stores `len|seq|payload` in a
  shared-memory SPSC ring with a 2-slot handshake; the backend-specific
  piece is just "ring a doorbell". That maps 1:1 to the Infineon IPC
  `Cy_IPC_Drv_ReleaseNotify()` primitive.
- **Zephyr-native** — `CONFIG_IPC_SERVICE=y` + `CONFIG_IPC_SERVICE_BACKEND_ICMSG=y`
  and the DT binding is standard `zephyr,ipc-icmsg`.

### 2.2 Why not OpenAMP

The sample samples under `zephyr/samples/subsys/ipc/openamp*` require:

- A remote-proc-like lifecycle (M33 loads M55 — PSE84 does this via
  `Cy_SysEnableCM55` in SoC bring-up, not via OpenAMP).
- A resource table in shared memory (the board's shared regions are
  4 KB each — fine for icmsg headers + small ring, tight for rpmsg).
- `libmetal` mbox / IPC driver — which we'd have to write anyway
  (Infineon doesn't ship one).

OpenAMP makes sense when the two sides negotiate multiple endpoints
dynamically. Here there's exactly one, and the framing is already
negotiated end-to-end.

### 2.3 Why not icbmsg / icmsg_me

- `icbmsg` (icmsg-with-buffers) adds a side-band buffer pool — useful
  for zero-copy large messages. Our frames are ≤247 B (L2CAP MTU), so
  inline is fine.
- `icmsg_me` is multi-endpoint — we have one.

**Decision:** `IPC_SERVICE_BACKEND_ICMSG`, single endpoint, 4 KB ring
per direction.

## 3. Cy_IPC_Drv_* channel selection

### 3.1 Available channels

From `modules/hal/infineon/mtb-dsl-pse8xxgp/pdl/devices/include/cy_device.h`:

- `CY_IPC_CHANNELS` = `CPUSS_IPC_NR` (IPC0, 8 channels) + `APPCPUSS_IPC_NR` (IPC1, 8 channels) = 16 total.
- `CY_IPC_CHAN_RRAM_LOCK` = 2 — reserved for RRAM driver mutex, **do not touch**.
- `CY_IPC_CHAN_USER` = `1u + IPC0_IPC_NR` — explicitly labelled in SDK
  header: *"First ipc channel index of IPC1 instance meant for
  CM33 <-> CM55"*. This is the start of the user-accessible range.
- `Cy_IPC_Pipe_Config()` (called from `soc_pse84_m55.c:43`) sets up the
  M55 side of the system pipe, which uses channels 0/1 of IPC0 (MXS22
  convention: `CY_IPC_CHAN_SYSCALL`, `CY_IPC_CHAN_SEMA`). **Also do
  not touch.**

### 3.2 Proposed channels for icmsg transport

| Role                     | Channel index   | Note                                           |
|--------------------------|-----------------|------------------------------------------------|
| M33 → M55 doorbell       | `CY_IPC_CHAN_USER`     (= IPC1 ch 0)   | "tx" ring was written, wake M55          |
| M55 → M33 doorbell       | `CY_IPC_CHAN_USER + 1` (= IPC1 ch 1)   | "rx" ring was written, wake M33          |

These are IPC1 (APPCPUSS) channels — the whole purpose of IPC1 per the
SDK comment is M33↔M55 application traffic. Channel 2 of IPC0 is
off-limits (`CY_IPC_CHAN_RRAM_LOCK`); PDL internals use IPC0 channels
0/1 for syscall + semaphore. We stay on IPC1 to avoid any collision.

### 3.3 The "notify" signalling primitive

We use message-less doorbells. The data lives in the shared-memory
ring; the IPC hardware is only used to wake the other core:

```c
/* M33 side, after writing to the tx ring */
Cy_IPC_Drv_ReleaseNotify(
    Cy_IPC_Drv_GetIpcBaseAddress(CY_IPC_CHAN_USER),
    1u << M55_IPC_INTR_IDX);

/* M55 side, ISR unmasks, then reads from the ring */
```

`Cy_IPC_Drv_SendMsgWord` / `SendMsgDWord` are **not used** — they write
data through the IPC hardware itself (32 or 64 bits per message,
hardware-gated). The ring is in SRAM and can carry arbitrary sizes;
IPC is strictly the wake signal.

## 4. Interrupt plan

### 4.1 IPC interrupt structures

Each IPC instance has N `IPC_INTR_STRUCT_Type` "interrupt structs"
that gate delivery to a given target core. The mapping is:

- **M55 target**: use an M55-visible IPC_INTR from IPC1 (`APPCPUSS`).
  Per `Cy_IPC_Drv_GetIntrBaseAddr(ipcIntrIndex)`, ipcIntrIndex
  `CY_IPC_INTERRUPTS_PER_INSTANCE..2*CY_IPC_INTERRUPTS_PER_INSTANCE-1`
  is the IPC1 range.
- **M33 target**: IPC0 interrupt struct (IPC0 is wired into M33 NVIC
  by ROM).

Proposal (to validate against `cy_device.h` macros during Phase 0b):

| Direction | Release-notify INTR struct | NVIC line (target) |
|-----------|----------------------------|--------------------|
| M33→M55   | IPC1 INTR_STRUCT #0        | `cpuss_interrupts_ipc_<N>` on M55 |
| M55→M33   | IPC0 INTR_STRUCT #0 (reuse syscall? NO — use a free one) | `cpuss_interrupts_ipc_<N>` on M33 |

`Q:` Exact INTR struct indices must be read from the PSE84 MTRM —
`CY_IPC_INTERRUPTS_PER_INSTANCE` is 16, plenty free.

### 4.2 ISR body

Kept as short as possible — acknowledge the IPC interrupt (so it
re-arms) and raise a Zephyr signal that the icmsg backend polls on
from a thread context:

```c
void ipc_rx_isr(const void *arg)
{
    const struct ifx_mbox_data *dev_data = arg;
    Cy_IPC_Drv_ClearInterrupt(dev_data->intr_base,
                              0u,                     /* release mask */
                              1u << dev_data->rx_ch); /* notify mask  */
    k_work_submit(&dev_data->rx_work);
}
```

The ring itself is SPSC (single-producer / single-consumer), so no
locking beyond `atomic` head/tail indices.

### 4.3 Priority

- M33 IPC ISR priority: mid-range. Must run lower than BT HCI UART
  RX ISR (which has a hard deadline for ACL flow control) but higher
  than the BT host thread. Candidate: `IRQ_PRIO_LOWEST - 2`.
- M55 IPC ISR priority: mid-range, below GFXSS DMA ISR, above PDM
  DMA ISR (PDM has its own ring so latency is forgiving).

`Q:` both exact priorities pin down in Phase 0b once HCI UART + GFXSS
DMA priorities are fixed.

## 5. Shared-memory layout

The board DTS already reserves 4 KB regions for M33↔M55 traffic
(`kit_pse84_eval_memory_map.dtsi:57-65`):

```
SHARED_MEMORY          0x240fe000  4 KB   (M33 NS view)
m55_allocatable_shared 0x240ff000  4 KB   (M55 view of its outbound ring)
```

Both regions are **SRAM0**, already MPC-attributed Non-Secure (so the
M33 Secure image must use the aliased NS address to access them).

### 5.1 Region assignment

```
+-----------------------------+--------+----------------+----------------------+
| Region                      | Addr   | Size           | Role                 |
+-----------------------------+--------+----------------+----------------------+
| M33 → M55 ring              | 0x240f | 4 KB           | Written by M33,      |
|                             | e000   |                | read by M55          |
+-----------------------------+--------+----------------+----------------------+
| M55 → M33 ring              | 0x240f | 4 KB           | Written by M55,      |
|                             | f000   |                | read by M33          |
+-----------------------------+--------+----------------+----------------------+
```

Both regions are shared with the existing `m33_allocatable_shared` and
`m55_allocatable_shared` DT nodes — **no DTS churn** required for the
regions themselves. The icmsg binding just references them by phandle.

### 5.2 Ring header (icmsg standard)

First 64 bytes of each region per `zephyr/include/zephyr/ipc/icmsg.h`:

```
offset 0x00 : u32 magic          (validated at open)
offset 0x04 : u32 tx_head
offset 0x08 : u32 rx_tail
offset 0x0c : u32 reserved
offset 0x10 : u8  data[N]        (4 KB - 0x10 = 4080 B payload ring)
```

4080 B of payload ring per direction — that's 16× L2CAP MTU-sized
(247 B) frames in flight before back-pressure. BT host handles
back-pressure via `bt_l2cap_chan_send` returning `-ENOBUFS`, which
propagates to the Python host via L2CAP credit flow control. Fine.

### 5.3 Cache hygiene

M55 has D-cache enabled (`SCB_EnableDCache()` in `soc_pse84_m55.c:33`).
M33 does not (not configured). Shared-memory writes from M55 need a
`DSB; clean-by-VA` before the doorbell; reads need an `invalidate-by-VA`
before reading the ring tail. The icmsg backend already handles this
via `sys_cache_data_flush_range` / `sys_cache_data_invd_range` when
`CONFIG_IPC_SERVICE_ICMSG_FLUSH_CACHE` is set. **Must enable this
Kconfig on the M55 image.**

`Q:` Does the M33 also need cache ops? It has no D-cache, so no — but
PSE84 has an AXI cache in front of SOCMEM. These regions are SRAM0
(`0x240x_xxxx`), which should bypass SMIF CACHE_BLOCK. **Verify during
Phase 0b** with a single oscilloscope / bus-analyser trace.

## 6. Zephyr mbox_driver_api surface

Icmsg talks to the hardware via `MBOX_DT_SPEC_GET` — Zephyr's generic
mailbox API. For PSE84 we need a new driver implementing
`mbox_driver_api`, since no existing Zephyr driver speaks
`Cy_IPC_Drv_*`.

### 6.1 DT binding: `infineon,pse84-mbox`

Proposed at `dts/bindings/mbox/infineon,pse84-mbox.yaml`:

```yaml
description: |
  Infineon PSE84 IPC-based mailbox. Wraps Cy_IPC_Drv_ReleaseNotify on
  send and the corresponding IPC_INTR_STRUCT release interrupt on
  receive. Each channel corresponds to one IPC hardware channel (from
  the CY_IPC_CHAN_USER range) + one IPC_INTR_STRUCT.

compatible: "infineon,pse84-mbox"

include: [mbox-controller.yaml, base.yaml]

properties:
  reg:
    required: true
    description: |
      Two cells: first = IPC channel index (0..15), second = IPC
      interrupt-struct index (0..15).

  interrupts:
    required: true
    description: IPC INTR release interrupt, targeted at *this* core.

  "#mbox-cells":
    const: 0
```

### 6.2 Application DT overlay (lives in the app, not the board)

```dts
/ {
    chosen {
        zephyr,ipc = &mbox_rx;
    };

    mbox_tx: mbox@1 {                 /* outbound: M55→M33 on M55 side */
        compatible = "infineon,pse84-mbox";
        reg = <9 0>;                  /* IPC channel 9 (USER+1), intr 0 */
        interrupts = <X IRQ_PRIO_LOWEST>;
        #mbox-cells = <0>;
        status = "okay";
    };

    mbox_rx: mbox@0 {                 /* inbound: M33→M55 on M55 side */
        compatible = "infineon,pse84-mbox";
        reg = <8 0>;                  /* IPC channel 8 (CY_IPC_CHAN_USER), intr 0 */
        interrupts = <Y IRQ_PRIO_LOWEST>;
        #mbox-cells = <0>;
        status = "okay";
    };

    ipc {
        ipc0: ipc0 {
            compatible = "zephyr,ipc-icmsg";
            tx-region = <&m55_allocatable_shared>;  /* 0x240ff000 */
            rx-region = <&m33_allocatable_shared>;  /* 0x240fe000 */
            mboxes = <&mbox_tx>, <&mbox_rx>;
            mbox-names = "tx", "rx";
            status = "okay";
        };
    };
};
```

The M33 image gets the mirror: tx-region swapped with rx-region.

### 6.3 Driver implementation shape (Phase 0b)

```
drivers/mbox/mbox_infineon_pse84.c
    - pse84_mbox_send()       -> Cy_IPC_Drv_ReleaseNotify
    - pse84_mbox_register_callback() -> stash cb in dev_data
    - pse84_mbox_mtu_get()    -> return 0  (doorbell-only, no data)
    - pse84_mbox_max_channels_get()  -> 1
    - ISR: Cy_IPC_Drv_ClearInterrupt -> invoke registered cb
```

Hard-wires a single channel per device — icmsg only needs "wake up"
semantics, not addressable mailboxes.

## 7. Open questions for Phase 0b review

1. **IPC_INTR_STRUCT index per core.** Which interrupt-struct indices
   are free for M33 and M55 targeting? PDL macro
   `Cy_IPC_Drv_GetIntrBaseAddr` takes an index; we need to confirm
   which indices already bind to SysCall / Semaphore (probably 0/1
   in IPC0) so our driver picks free ones in IPC1. Read the PSE84 MTRM
   `IPC Interrupt Cross-bar` section.
2. **NVIC IRQ number**. Each IPC_INTR_STRUCT maps to a specific NVIC
   line in the DWT. The DTS needs an exact number in
   `interrupts = <N ...>`. Get it from
   `modules/hal/infineon/mtb-dsl-pse8xxgp/pdl/devices/include/pse84_cpuss_irq.h`.
3. **TF-M secure attribution.** The M33 runs Secure. If Phase 4 moves
   to `_m33_ns`, IPC1 channels and INTR structs need attribution
   in `pse84_s_protection.c`. Currently every peripheral region is
   re-attributed NON_SECURE / NONPRIV by `cy_ppc_unsecure_init`, so
   APPCPUSS IPC access from M33 Secure is *impossible* after
   `soc_late_init_hook` returns. **This is the same root cause as
   the M33_HARDFAULT_RCA.md** — fixing that (Option A: `__disable_irq`
   before PPC) unblocks IPC. If we instead tighten PPC to leave
   APPCPUSS Secure for M33, IPC and HCI UART work without masking
   interrupts.
4. **Cache policy on the M55 side.** Is `CONFIG_IPC_SERVICE_ICMSG_FLUSH_CACHE`
   actually correct for SRAM0 at 0x240f_xxxx, or does PSE84 have a
   SOCMEM-aliased view that needs different ops? Trace one transaction
   with a bus analyser.
5. **Ring size trade-off.** 4 KB gives 16 frames in flight. When we
   stream TTS Opus back (Phase 8), latency budget may want 32+ frames
   — is there a 2nd pair of 4 KB regions available elsewhere in SRAM0?
6. **Framing-layer error injection.** Master plan says M33 is a dumb
   forwarder; but what if IPC corrupts a frame mid-transfer (SEU in
   SRAM)? icmsg has a per-message checksum — is that enough, or do we
   want an additional framing CRC? Probably enough (24 h of streaming
   at 16 kbps = ~1.5 GB moved; SEU rate on SRAM0 is ~0 at room temp).
7. **Sysbuild image split for the M33 BT image** — the current
   `enable_cm55` companion is `samples/basic/minimal` with one
   Kconfig override. Phase 4 replaces it with the BT host image; the
   new image needs the mbox driver, icmsg, and the BT host. Sysbuild
   `ExternalZephyrProject_Add` can point at an in-repo app dir —
   propose `pse84_assistant_m33/` as a sibling of this app.

## 8. References

- `zephyr/include/zephyr/ipc/icmsg.h`
- `zephyr/subsys/ipc/ipc_service/backends/icmsg.c`
- `zephyr/dts/bindings/ipc/zephyr,ipc-icmsg.yaml`
- `zephyr/dts/bindings/mbox/mbox-controller.yaml`
- `modules/hal/infineon/mtb-dsl-pse8xxgp/pdl/drivers/include/cy_ipc_drv.h`
- `modules/hal/infineon/mtb-dsl-pse8xxgp/pdl/devices/include/cy_device.h`
  (CY_IPC_CHAN_USER definition, channel counts)
- `zephyr/boards/infineon/kit_pse84_eval/kit_pse84_eval_memory_map.dtsi`
  (SHARED_MEMORY reservations)
- Phase 4 scouting: `zephyr_workspace/pse84_assistant_phase4_scouting.md`
