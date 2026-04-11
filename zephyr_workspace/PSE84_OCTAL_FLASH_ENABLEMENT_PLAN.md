# PSE84 Octal Flash (S28HS01GTGZBHI030) Zephyr Upstream Enablement Plan

Board: **KIT_PSE84_EVAL** (PSE846GPS2DBZC4A)
Target flash: **Infineon S28HS01GTGZBHI030** — 1 Gb / 128 MB SEMPER Octal NOR on SMIF0 CS0 (`smif0_spihb_select0`)
Currently enabled flash: S25FS128S (16 MB Quad) on SMIF0 CS1 (`smif0_spihb_select1`)
Branch under development: `pse84-gfxss-display-driver` on remote `fork`

## 1. State of the Zephyr SMIF / flash driver stack (is Octal supported?)

Short answer: **the PDL and the serial-memory helper already support Octal**, but nothing in the Zephyr tree is wired up to use it. The gap is entirely in (a) the board DT, (b) the Zephyr flash driver glue, and (c) the cycfg_qspi_memslot C stub that declares which memories exist.

Concrete evidence:

- `zephyr/drivers/flash/CMakeLists.txt` lines 93-97 selects `flash_infineon_serial_memory_qspi.c` for `CONFIG_SOC_FAMILY_INFINEON_EDGE` when `CONFIG_INFINEON_CAT1_QSPI_FLASH` is set; otherwise falls back to `flash_infineon_qspi.c` (legacy HAL). PSE84 goes through the serial-memory path.
- `zephyr/drivers/flash/flash_infineon_serial_memory_qspi.c`:
  - `DT_DRV_COMPAT` is `infineon_qspi_flash` (line 8).
  - `ifx_serial_memory_flash_init()` at line 237 calls `mtb_serial_memory_setup(..., MTB_SERIAL_MEMORY_CHIP_SELECT_1, SMIF0_CORE0, ...)` — **chip select 1 is hardcoded** (line 244). Whatever DT says, this driver only ever talks to the QSPI at CS1.
  - It references the symbol `smif0BlockConfig` via `extern` (line 27). That symbol comes from the next layer below.
- `zephyr/modules/hal_infineon/zephyr-ifx-cycfg/kit_pse84_eval/cycfg_qspi_memslot.c`:
  - Only one memory configured, `S25FS128S_SMIF0_SlaveSlot_1`, at `.slaveSelect = CY_SMIF_SLAVE_SELECT_1`, `.baseAddress = 0x60000000U`, `.memMappedSize = 0x1000000U` (lines 475-523).
  - `smif0MemConfigs[CY_SMIF_DEVICE_NUM0]` has only one entry (line 510), and `CY_SMIF_DEVICE_NUM0 == 1` in the header. `smif1BlockConfig.memCount = 0` — SMIF1 is empty.
  - The read command uses `.cmdWidth = CY_SMIF_WIDTH_SINGLE`, `.addrWidth = CY_SMIF_WIDTH_QUAD`, `.dataWidth = CY_SMIF_WIDTH_QUAD`, `.dataRate = CY_SMIF_SDR` — i.e. 1-4-4 SDR, the S25FS128S "Quad read" mode.
- `zephyr/dts/bindings/flash_controller/infineon,qspi-flash.yaml`: a 5-line stub that only declares `compatible: "infineon,qspi-flash"` and `include: flash-controller.yaml`. No properties for io-mode, chip-select, frequency, or RX capture mode.
- `zephyr/boards/infineon/kit_pse84_eval/kit_pse84_eval_memory_map.dtsi` lines 95-135: `flash_controller@40250000` is `infineon,qspi-flash`, child `flash0@8000000` with `reg = <0x08000000 DT_SIZE_M(16)>`. The size and base address are hardcoded for the 16 MB Quad chip.
- **No** `smif0`/`smif1` nodes exist in the PSE84 SoC dtsi (`dts/arm/infineon/edge/pse84/pse84.dtsi`, `pse84.cm33.dtsi`). The SMIF controller appears only in the board memory_map.
- **SMIF IP version is v6** (`modules/hal/infineon/mtb-dsl-pse8xxgp/pdl/devices/include/ip/cyip_smif_v6.h` exists). That matters because `mtb_serial_memory.c`'s octal code is gated on `CY_IP_MXSMIF_VERSION >= 4`.
- `zephyr/modules/hal_infineon/serial-memory/source/mtb_serial_memory.c` already has the Octal plumbing: `_mtb_serial_memory_enable_octal_mode_if_needed()` (lines 247-293) calls `Cy_SMIF_MemOctalEnable()`, then sets `rx_capture_mode` to `CY_SMIF_SEL_XSPI_HYPERBUS_WITH_DQS` for DDR or `CY_SMIF_SEL_NORMAL_SPI` for SDR. This is invoked unconditionally from `mtb_serial_memory_setup()` at line 483, gated on SMIF v4+. All we need is a `cy_stc_smif_mem_config_t` whose `readCmd->dataWidth == CY_SMIF_WIDTH_OCTAL` and whose `deviceCfg->writeStsRegOeCmd != NULL`.
- `Cy_SMIF_MemOctalEnable()` is defined at `modules/hal/infineon/mtb-dsl-pse8xxgp/pdl/drivers/source/cy_smif_memslot.c` line 614. `CY_SMIF_WIDTH_OCTAL == 3U` is declared at `modules/hal/infineon/mtb-dsl-pse8xxgp/pdl/drivers/include/cy_smif.h` line 516.

**Gap summary:** The HAL stack supports Octal. The Zephyr driver and cycfg stub do not configure Octal and do not let DT pick the chip-select. Four things must change together: (a) a new S28HS01GT memslot block in cycfg_qspi_memslot.c, (b) DT-driven chip-select selection in `flash_infineon_serial_memory_qspi.c`, (c) binding properties to express chip-select / io-mode / data-rate, (d) the board memory map to point at the Octal device's size and aliases.

## 2. What the ModusToolbox SmartwatchDemo actually does

I read the full BSP at `/Users/danahern/code/claude/embedded/zephyr_workspace/SmartwatchDemo_Edit/bsps/TARGET_APP_KIT_PSE84_EVAL_EPC2/`. **The MTB SmartwatchDemo does not use the Octal flash.** It uses the same 16 MB S25FS128S Quad as the current Zephyr port.

Evidence:

- `config/design.cyqspi`: slot 0 is `MemoryId = "Not used"`, `DataSelect = OCTAL_SPI_DATA_0_7`, aperture `0x60000000-0x61FFFFFF` (2 MB — this is just a reservation, not a populated 128 MB region). Slot 1 is `MemoryId = "S25FS128S (Hybrid at bottom, 64 KB sectors elsewhere)"`, `DataSelect = QUAD_SPI_DATA_0_3`, aperture `0x60000000-0x60FFFFFF` (16 MB), `WriteEnable = true`, `MergeTimeout = CY_SMIF_MERGE_TIMEOUT_256_CYCLES`. (Note: slot 0 and slot 1 both start at 0x60000000 in this XML — SMIF0 only actually maps slot 1 at runtime.)
- `config/GeneratedSource/cycfg_qspi_memslot.c` line 494: `.slaveSelect = CY_SMIF_SLAVE_SELECT_1`, line 505: `.baseAddress = 0x60000000U`, line 508: `.memMappedSize = 0x1000000U`. Only `S25FS128S_SMIF0_SlaveSlot_1` is declared, and `CY_SMIF_DEVICE_NUM0 == 1`.
- `config/GeneratedSource/cycfg_memory_types.h` only defines `IFX_MEMORY_TYPE_SMIF0MEM1`. No SMIF0MEM0.
- `config/GeneratedSource/device_mem.json` line 10: `SMIF0MEM1 size 0x01000000 starts [0x60000000, 0x08000000, 0x18000000, 0x70000000]`. The four aliases (NS SBUS, NS CBUS, Secure CBUS, Secure SBUS) apply to whichever device is installed at SMIF0 — they are SMIF port0 aliases, not per-CS.
- `config/GeneratedSource/cycfg_peripherals.c` lines 224-238: `rx_capture_mode = CY_SMIF_SEL_NORMAL_SPI`, which is the SDR Quad capture mode, not the DDR `CY_SMIF_SEL_XSPI_HYPERBUS_WITH_DQS` mode that Octal DDR requires.
- `config/GeneratedSource/cymem_gnu_CM33_0.ld` lines 98-108: M33S image linked at `m33s_nvm : ORIGIN = 0x60100000, LENGTH = 0x00200000`, `m33_nvm : 0x60340000, 0x00200000`, `m55_nvm : 0x60580000, 0x002C0000`. These exact offsets match what Zephyr's `kit_pse84_eval_memory_map.dtsi` uses (0x60100000 for `m33s_header`, 0x60500000 for `m55_xip`). **Both Zephyr and the MTB SmartwatchDemo share the same ROM-boot layout sized for the 16 MB Quad.**
- `config/GeneratedSource/cycfg_peripherals.c` line 376-390: `init_cycfg_peripherals()` only calls `Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SMIF0_...)` — it does **not** call `Cy_SMIF_Init()` or `Cy_SMIF_Memslot_Init()`. Application code inherits whatever the Extended Boot ROM left configured.
- `/Users/danahern/code/claude/embedded/zephyr_workspace/SmartwatchDemo_Edit/openocd.tcl` passes `QSPI_FLASHLOADER=bsps/.../config/GeneratedSource/PSE84_SMIF.FLM` — the same 488 KB CMSIS flash loader that ships in `modules/hal/infineon/mtb-template-pse8xxgp/files/flashloader/PSE84_SMIF.FLM`.

**Conclusion:** There is no upstream MTB recipe to copy. The SmartwatchDemo is a 16 MB Quad image and will need to be redone from scratch as an Octal image. This is a real risk: there is no known-good MTB reference config for OSPI boot on this dev kit to validate our Zephyr implementation against. We may need to produce and validate the OSPI memory-mapped config ourselves, before Zephyr can rely on it.

## 3. Secure Enclave / Extended Boot ROM state before application code runs

What I can verify from the tree:

- `zephyr/soc/infineon/edge/pse84/pse84_metadata.cmake` (added in commit 165f2e1c84f, "Remove dependency on edgeprotecttools") signs the M33 secure hex with `imgtool sign` using `header_addr = dt_reg_addr(m33s_header)` and `header_size = dt_reg_size(m33s_header)`. The Extended Boot ROM reads the signed header at that physical address (0x60100000 in the current DT) and jumps to the M33S vector table. `header_addr` comes from DT.
- `zephyr/modules/hal_infineon/CMakeLists.txt` for `SOC_FAMILY_INFINEON_EDGE` does **not** compile a pre-`main()` SMIF init function; it only adds mtb-srf, serial-memory, zephyr-ifx-cycfg, and the PDL. The first SMIF-touching code Zephyr runs is `ifx_serial_memory_flash_init()` (driver POST_KERNEL). By then, XIP from m33s_xip has already happened.
- The MTB BSP confirms the same: `cybsp.c::cybsp_init()` calls `init_cycfg_all()`, which calls `init_cycfg_peripherals()`, which only sets SMIF0's clock gate. **No application-side SMIF re-init is performed on PSE84.** The ROM's SMIF configuration is inherited and used directly for XIP.

This means: **the Extended Boot ROM's SMIF0 configuration is authoritative for code execution.** Whatever the ROM configured is what the M33 secure image runs on. If the ROM is configured for Quad-1-4-4-SDR, attempting to "switch" to Octal DDR from application code would require:
(a) copying the running code into SRAM before touching SMIF0 control registers (since reconfiguration bus-faults any live XIP), and
(b) bringing SMIF0 back up in Octal mode, and
(c) confirming the memory-mapped aperture is re-enabled.

What I could not verify from the open repo:

- Exactly **what mechanism configures the Extended Boot ROM's SMIF init** — the candidates are (i) OTP/eFuse bits programmed at provisioning time, (ii) an Infineon "edgeprotecttools" policy/asset that ships a boot-descriptor object into RRAM which the SE reads, (iii) a pre-main code block in RRAM at 0x22000000 (`extended_boot_reserved 0x00011000` is reserved in device_mem.json) that the ROM executes, (iv) an SE-config-table loaded over debug protocol. The commit history message for 165f2e1c84f ("Remove dependency on edgeprotecttools") implies edgeprotecttools did something security-sensitive that Zephyr has stepped away from — very likely this is where the SMIF-boot config normally lives.
- Whether the ROM auto-detects a ready flash device (reading SFDP from CS0/CS1 over single-SPI) or requires explicit asset-table configuration.

This is the single biggest unknown in the plan. **Before we do any DT/driver work we need to confirm the ROM-SMIF-config story with Infineon** (see section 10).

## 4. Draft binding and driver extension

I recommend a **minimal, layered** change rather than a parallel new driver.

### 4a. New DT binding properties on `infineon,qspi-flash`

Rename is probably unnecessary — the compat already says "qspi" but maps to SMIF on PSE84, which covers Octal on the same IP. Adding properties is upward-compatible. Path: `dts/bindings/flash_controller/infineon,qspi-flash.yaml`. Proposed properties:

- `chip-select` (int, default 0): 0..3, which `CY_SMIF_SLAVE_SELECT_N` to use. Drives the hardcoded value in the driver.
- `io-mode` (string, default `"quad"`): `"single" | "dual" | "quad" | "octal"`. Maps to `CY_SMIF_WIDTH_*` for `readCmd.dataWidth` and `programCmd.dataWidth`.
- `data-rate` (string, default `"sdr"`): `"sdr" | "ddr"`. Maps to `CY_SMIF_SDR` / `CY_SMIF_DDR`.
- `rx-capture-mode` (string, default `"normal-spi"`): `"normal-spi" | "normal-spi-with-dlp" | "xspi-hyperbus-with-dqs"`. Maps to `cy_en_smif_capture_mode_t`.
- `smif-frequency` (int, default 50000000): clock after the SMIF v2+ internal /2 divide (i.e. the pin frequency).
- `jedec-id` (array-8, optional): S28HS01GT is `0x34 2b 1c` (JEDEC vendor Infineon, device S28HS01GT). Useful for runtime verification.

### 4b. New devicetree nodes

Add a real SMIF controller node in the SoC tree at `dts/arm/infineon/edge/pse84/pse84.dtsi`:

```dts
smif0: flash_controller@40250000 {
    compatible = "infineon,qspi-flash";
    reg = <0x40250000 0x10000>;
    #address-cells = <1>;
    #size-cells = <1>;
    status = "disabled";
};
```

In `boards/infineon/kit_pse84_eval/kit_pse84_eval_memory_map.dtsi` replace the current anonymous `flash_controller@40250000` with:

```dts
&smif0 {
    status = "okay";

    flash0: flash0@8000000 {
        compatible = "soc-nv-flash";
        reg = <0x08000000 DT_SIZE_M(128)>;
        write-block-size = <512>; /* S28HS page = 512 bytes in OPI mode */
        erase-block-size = <DT_SIZE_K(256)>; /* S28HS uniform 256 KB sectors */

        chip-select = <0>;
        io-mode = "octal";
        data-rate = "ddr";
        rx-capture-mode = "xspi-hyperbus-with-dqs";
        smif-frequency = <200000000>;

        partitions {
            /* see section 5 */
        };
    };
};
```

Note the page-size / sector-size change: S28HS01GT default uniform sector is 256 KB, page 512 bytes in OPI; in legacy SPI it is 64 KB / 256 bytes. Erase granularity of 256 KB will affect partition alignment — storage partitions must be sized to a multiple of 256 KB.

### 4c. Driver extension

At `drivers/flash/flash_infineon_serial_memory_qspi.c`:

- Replace the hardcoded `MTB_SERIAL_MEMORY_CHIP_SELECT_1` at line 244 with `DT_INST_PROP(0, chip_select)` mapped through a chip-select lookup (0→`MTB_SERIAL_MEMORY_CHIP_SELECT_0`, 1→`_1`, etc.).
- Add a build-assert that `DT_PROP(SOC_NV_FLASH_NODE, write_block_size)` matches the memslot programCmd's expected page size, and similarly erase-block-size.
- Keep the driver dependent on the external `smif0BlockConfig` coming from zephyr-ifx-cycfg — do not try to build a new memslot from DT properties at compile time. Instead, expose a compile-time switch that selects between the existing S25FS128S memslot and a new S28HS01GT memslot.

### 4d. New cycfg_qspi_memslot entry

The invasive change is in `modules/hal_infineon/zephyr-ifx-cycfg/kit_pse84_eval/cycfg_qspi_memslot.{c,h}`. Write (or generate with the QSPI Configurator) a new file pair or a Kconfig-guarded alternate block that declares a second memory, `S28HS01GT_SMIF0_SlaveSlot_0`, with:

- `.slaveSelect = CY_SMIF_SLAVE_SELECT_0`
- `.dataSelect = CY_SMIF_DATA_SEL0` (DQ0..7 → SMIF0 pins 0..7)
- `.baseAddress = 0x60000000U`
- `.memMappedSize = 0x08000000U` (128 MB)
- `.flags = CY_SMIF_FLAG_SMIF_REV_3 | CY_SMIF_FLAG_MEMORY_MAPPED | CY_SMIF_FLAG_WR_EN | CY_SMIF_FLAG_MERGE_ENABLE`
- `.mergeTimeout = CY_SMIF_MERGE_TIMEOUT_256_CYCLES`
- `.deviceCfg = &deviceCfg_S28HS01GT_SMIF0_SlaveSlot_0`

And a `deviceCfg_S28HS01GT_SMIF0_SlaveSlot_0` with:
- Read command: `command = 0xEE` (OPI DDR Fast Read), `cmdWidth/addrWidth/dataWidth = CY_SMIF_WIDTH_OCTAL`, `cmdRate/addrRate/dataRate = CY_SMIF_DDR`, `dummyCycles = 20` (Infineon default for DDR 200 MHz), `cmdPresence = CY_SMIF_PRESENT_2BYTE` (OPI DDR uses 2-byte commands — the `commandH` field holds the complement byte).
- Program command: `0x12` (OPI DDR Page Program), `CY_SMIF_WIDTH_OCTAL`, `CY_SMIF_DDR`.
- Write-enable: `0x06` in OPI mode (single-byte cmd width `CY_SMIF_WIDTH_OCTAL`, DDR).
- Sector erase: `0x21` (4-byte addressing, 256 KB erase unit).
- Chip erase: `0xC7`.
- Status register reads: `readStsRegWipCmd = 0x05`, `readStsRegOeCmd` / `writeStsRegOeCmd` set to the S28HS01GT OCTAL-enable register pair (`CFR5V` = `0x00800005`, OE bit 3 in status register 2 — see datasheet, "Spansion/Infineon SEMPER OPI enable sequence").
- `octalEnableRegAddr` / `stsRegOctalEnableMask`: configure so `_mtb_serial_memory_enable_octal_mode_if_needed()` sends the correct one-byte sequence.
- `hybridRegionInfo = NULL` (S28HS01GT is uniform 256 KB sectors, not a hybrid layout like the S25FS128S).

The key detail the plan must flag for verification: **the Infineon QSPI Configurator ships with a pre-canned "S28HS01GT" memory XML** in its memory database. The simplest, lowest-risk path is to run that configurator against a copy of `design.cyqspi`, set Slot 0 to `MemoryId = "S28HS01GT..."`, `DataSelect = OCTAL_SPI_DATA_0_7`, size `0x8000000`, and **copy the generated cycfg_qspi_memslot.c block** into the Zephyr-side file. Do not hand-write these fields.

## 5. Partition layout migration (16 MB → 128 MB)

Current layout in `kit_pse84_eval_memory_map.dtsi`:

| Partition | Offset | Size |
|---|---|---|
| `storage_partition` | 0 | 1 MB |
| `m33s_header` | 0x60100000 | 0x400 |
| `m33s_xip` | 0x18100400 | 2304 KB |
| `m33_xip` | 0x8340400 | 0x1bfc00 (≈1.79 MB) |
| `m55_xip` | 0x60500000 | 2 MB |

Note three things about the existing layout:
1. The Zephyr DT uses **mixed address aliases**: `m33s_xip` is at `0x18100400` (secure CBUS), `m33_xip` at `0x08340400` (NS CBUS), `m55_xip` at `0x60500000` (NS SBUS). This is fragile — it works because all four aliases resolve to the same 16 MB aperture inside SMIF0. Moving to Octal does not change aliasing; we will reuse these same alias prefixes but with a 128 MB aperture.
2. The offsets imply `m33s_header` starts at 0x100000 inside the flash aperture (i.e. 1 MB from SMIF0 base), leaving the first 1 MB for `storage_partition`. Same pattern as the MTB demo.
3. The erase granularity constraint matters: on the current S25FS128S the "hybrid" region has 4 KB sectors at the bottom for `storage_partition`. **S28HS01GT is uniform 256 KB sectors in OPI mode.** The `storage_partition` size must become a multiple of 256 KB and any per-page write semantics above storage must tolerate a 256 KB minimum erase.

Proposed 128 MB layout (all offsets shown in NS SBUS alias 0x60000000 for clarity; the DT will use the same mixed aliases as today to keep linker scripts unchanged):

| Partition | NS SBUS | Size | Notes |
|---|---|---|---|
| `storage_partition` | 0x60000000 | 1 MB | Must be ≥256 KB and 256 KB-aligned. Consider 4 MB for LittleFS headroom. |
| `m33s_header` | 0x60100000 | 1 KB (0x400) | Same address as today — keeps ROM boot lookup unchanged. Sits inside a 256 KB erase unit that also holds the start of m33s_xip. |
| `m33s_xip` | 0x60100400 | 2.25 MB | Rounded up to 256 KB boundary. |
| `m33_xip` | 0x60380000 | 2 MB | Up from 1.79 MB, aligned to 256 KB. |
| `m55_xip` | 0x60580000 | 8 MB | Large headroom for the LVGL smartwatch app and GFXSS render code. |
| `lvgl_assets` | 0x60D80000 | 32 MB | Read-only LVGL image/animation blob, mapped as an fstab entry or as a partition the app reads directly via XIP. |
| `animation_data` | 0x62D80000 | 16 MB | Read-only video/animation assets. |
| `ota_slot_primary` | 0x63D80000 | 16 MB | MCUboot primary slot for future OTA. |
| `ota_slot_secondary` | 0x64D80000 | 16 MB | MCUboot secondary slot. |
| `reserved_end` | 0x65D80000 | ~35 MB | Unused, available for future expansion. |

Design decisions:
- `m33s_header` stays at **0x60100000** (verify offsets). If the ROM's SMIF initialization ends up mapping the octal flash at a different base or the 4-alias layout shifts, all partition offsets must follow. This is why the address of `m33s_header` must remain the single source of truth — do not hardcode it in any linker script.
- The partition table should move to `partitions/` syntax and use symbolic names, then the `pse84_metadata.cmake` script picks up `dt_reg_addr(m33s_header)` automatically (already does, thanks to commit 165f2e1c84f).
- `m55_xip` must stay in a contiguous linear region the M55 linker can resolve at link time. Bumping it to 8 MB requires the M55 linker script to be regenerated from DT.
- OTA slots should only be added in a second PR once a working boot-from-octal is proven with no OTA layer. Adding MCUboot on day one increases the number of variables.

## 6. Flashing pipeline

The current flash path is CMSIS-flashloader based:
- `zephyr/boards/infineon/kit_pse84_eval/support/qspi_config.cfg` line 6-8: `set SMIF_BANKS { 1 {addr 0x60000000 size 0x1000000 psize 0x0000100 esize 0x0010000} }` — a single bank at CS1, 16 MB, 256-byte program, 64 KB erase. OpenOCD reads this and declares a flash bank.
- `zephyr/boards/infineon/kit_pse84_eval/support/openocd.cfg` sources `target/infineon/pse84xgxs2.cfg` from the Infineon-provided OpenOCD build. That target cfg is **not** in the Zephyr tree or homebrew OpenOCD (I searched — only tle987x.cfg ships in homebrew openocd). The user must be running an Infineon-patched OpenOCD that knows about PSE84 and can load CMSIS FLM algorithms.
- `modules/hal_infineon/mtb-template-pse8xxgp/files/flashloader/PSE84_SMIF.FLM` (488 KB) and `FlashPSE84_SMIF.out` (829 KB) are the CMSIS flashloader binaries. The MTB SmartwatchDemo references the same FLM in its `openocd.tcl`.

For Octal enablement, the pipeline needs:

1. **A new FLM that programs the Octal flash.** The existing `PSE84_SMIF.FLM` is 488 KB of compiled code and I cannot inspect whether it supports both Quad and Octal without extracting the ELF and looking at its `FlashDevice` descriptor array. CMSIS flashloaders declare a single `FlashDevice` struct describing the device geometry — the `DevName`, `DevSize`, `PageSize`, `sectors[]` fields are what OpenOCD reads. **It is likely the existing FLM only knows about the 16 MB S25FS128S**, because (a) the hal_infineon version.xml is 1.1.1.703 and matches the SmartwatchDemo 1.1.1.703, and (b) the MTB SmartwatchDemo's design.cyqspi uses slot 1 only. **Assume we need a new FLM.**
2. Options for producing the new FLM:
   a. **Ask Infineon for a pre-built PSE84_SMIF_OCTAL.FLM.** Highest-confidence path. They have the memory database and the flashloader build toolchain. See section 10.
   b. **Use the ModusToolbox Device Configurator / QSPI Configurator to regenerate the FLM.** MTB 3.7+ knows how to emit an Octal flashloader from a `design.cyqspi` that selects the S28HS01GT on slot 0 with OCTAL_SPI_DATA_0_7. This is the likely path — `bsps/.../config/FlashLoaders/` in the MTB SmartwatchDemo is exactly where MTB would drop the regenerated FLM.
   c. **Hand-write a FLM in C** following the Keil CMSIS-Pack format. This is a significant chunk of work (2-3 weeks) and carries correctness risk.
3. Update `boards/infineon/kit_pse84_eval/support/qspi_config.cfg` to declare the new bank:
   ```tcl
   set SMIF_BANKS {
     0 {addr 0x60000000 size 0x8000000 psize 0x0000200 esize 0x0040000}
   }
   ```
   Note the change of bank index (0 not 1), size (128 MB), page (512 bytes), sector (256 KB).
4. Update `boards/infineon/kit_pse84_eval/support/openocd.cfg` (or a parallel `openocd_octal.cfg`) to point at the new FLM file. Pick the FLM path by `CONFIG_INFINEON_SMIF_OCTAL` or by a new Kconfig knob.
5. The `board.cmake` rewiring should be minimal — the existing signed-hex flow (`pse84_metadata.cmake`) is DT-driven by `m33s_header` reg, so it will automatically produce a hex whose load addresses match whatever the Octal DT says. The M33 NS TF-M merged hex (`tfm_merged.hex`) will likewise follow DT.

**One bootstrapping wrinkle.** The FLM runs on the M33 (or M0+?) at some SRAM address and drives SMIF directly. If the FLM does a full Octal-enable sequence on every program cycle, it will leave the chip in Octal mode, which is fine. If it only talks single-SPI and relies on the chip already being in Octal mode, we have a chicken-and-egg problem. This is a question to ask Infineon: **does the flashloader put the chip into Octal mode itself, or does it expect the SE/ROM to have done so?**

## 7. Boot chain verification — minimum viable test

Sequenced smallest-to-largest:

1. **Read-only XIP smoke test.** Without reflashing, write a test application that does not write to flash, just reads the JEDEC ID via a SMIF transfer and prints it, after booting from the existing Quad. This verifies the driver's DT plumbing changes compile. The app should still boot from the Quad if `chip-select = <1>, io-mode = "quad"` in DT. No behavioral change.
2. **Octal config, single-CS, program-then-read test.** Replace the Zephyr board memory map with the 128 MB Octal DT from section 4. Flash with the new FLM. The M33S image lands at 0x60100000 on the Octal chip. Boot. Confirm the console prints. Confirm `flash_read(flash_device_get_binding("flash-controller@40250000"), 0x1000, buf, 64)` returns the expected bytes.
3. **Multi-core boot.** Enable `SB_CONFIG_BOARD_KIT_PSE84_EVAL_PSE846GPS2DBZC4A_M55=y` so sysbuild pulls in the M55 enable-cm55 helper. Flash the M55 image into `m55_xip` at 0x60580000 on the Octal. Confirm M33 brings up M55 and the M55 hello-world runs from XIP.
4. **Display pipeline smoke.** Run the `samples/drivers/display` sample with the PSE84 GFXSS driver. This exercises the SOCMEM-FB region (unchanged) plus XIP from the Octal. If display comes up and LVGL renders, the Octal XIP path is stable across the concurrent SMIF0 reads from both cores.
5. **Storage write cycle.** Write a pattern to `storage_partition`, power-cycle, read it back. Confirms the program and erase paths in the driver are honoring the 256 KB erase block correctly.

Pass criteria: all five tests in sequence, no bus faults, no SMIF MMIO-mode errors in the log, no flash checksum mismatches on the M33S signed header.

## 8. Risk assessment and fallback

**High risk.**
- **ROM-SMIF-config is not under our control from Zephyr source.** If the SE/ROM is pinned by eFuse to Quad-1-4-4 and there's no Infineon-supported path to reconfigure it, XIP from Octal is impossible without physically re-provisioning the device. The worst case: we spend two weeks on DT/driver work only to discover the ROM cannot boot from OPI on this silicon revision. Mitigation: get written confirmation from Infineon before any code work starts.
- **Existing PSE84_SMIF.FLM only supports the Quad device.** Without a confirmed Octal FLM (from Infineon or MTB Configurator), we cannot even program the Octal chip to test. Mitigation: spike step — ask Infineon for the Octal FLM as step zero.
- **S28HS01GT Octal DDR requires 200 MHz SMIF clock with `xspi-hyperbus-with-dqs` RX capture.** The DLL calibration code in `cy_smif.c` must train correctly at 200 MHz or reads will be unreliable. The MTB SmartwatchDemo runs Quad at 100 MHz (`CY_SMIF_100MHZ_OPERATION`), so there is no local validation precedent for 200 MHz DLL calibration. Mitigation: start at 50 MHz SDR Octal (1-8-8), which does not need DQS/DLL calibration, and only push to DDR after the base path works.
- **DQ0..3 shared between Quad and Octal.** The two CS signals both drive the same data lines. If the Octal memslot declaration accidentally leaves `SMIF0_MEM_SLOT_1` enabled with `flags = CY_SMIF_FLAG_MEMORY_MAPPED`, SMIF0 may attempt to drive both chips on the same read, which will almost certainly hang the bus. Mitigation: the new cycfg_qspi_memslot.c must have `CY_SMIF_DEVICE_NUM0 == 1` with the sole entry being S28HS01GT_SlaveSlot_0 — no S25FS128S block at all in the Octal build. Guard with Kconfig so the two variants cannot coexist at link time.
- **Signed M33 image verification.** If `m33s_header` moves by even one byte, the ROM's signature verification fails and the board will not boot. Mitigation: keep the 0x60100000 address unchanged and only grow the partitions below.

**Medium risk.**
- The 256 KB minimum erase block will break existing storage/LittleFS users who assume 4 KB erase. Mitigation: make `storage_partition` size >= 1 MB and document in board notes.
- The new binding properties will break out-of-tree boards using `infineon,qspi-flash` if they relied on the hardcoded CS1 behavior. Mitigation: default the new `chip-select` property to 1 and `io-mode` to "quad", matching legacy.

**Low risk.**
- DT alias chaos. Straightforward to audit.

**Rollback plan.** All changes are confined to: `kit_pse84_eval_memory_map.dtsi`, `flash_infineon_serial_memory_qspi.c`, `infineon,qspi-flash.yaml`, `cycfg_qspi_memslot.{c,h}`, `qspi_config.cfg`. Revert any or all of these to restore the Quad boot. Keep the existing Quad FLM in the tree; keep the existing QSPI `qspi_config.cfg`; gate all new code behind `CONFIG_INFINEON_SMIF_OCTAL` defaulting `n`. A user who wants the old Quad boot sets `CONFIG_INFINEON_SMIF_OCTAL=n` and everything reverts compile-time. Physical rollback: re-flash the Quad chip with the old FLM — the Octal chip can stay powered but idle as long as the DT does not declare it.

## 9. Scope estimate

Rough breakdown (single engineer, assuming no Infineon blocking questions):

| Work unit | Effort | PR |
|---|---|---|
| Binding additions + driver DT-plumbing | 2-3 days | PR 1 (Zephyr) |
| New cycfg_qspi_memslot S28HS01GT block | 2-3 days | PR 2 (hal_infineon west module) |
| Board DT update + partition migration | 1-2 days | PR 1 (Zephyr, bundled) |
| OpenOCD qspi_config + FLM wiring | 1 day | PR 1 (Zephyr, bundled) |
| Hardware bring-up + debug | 3-7 days | — (during PR 1 review cycle) |
| Kconfig guard + default=n | 0.5 day | PR 1 (Zephyr, bundled) |
| Docs update (board/index.rst, release notes) | 1 day | PR 1 (Zephyr, bundled) |

**Total: ≈2.5 weeks if Infineon answers questions quickly and the first-try Octal config is close. 4-6 weeks if we hit the ROM-init or FLM issues first.**

**PR topology:**
- **PR 1** (Zephyr upstream): binding change, driver CS parameterization, new board memory map, OpenOCD bank config, Kconfig gate, docs. Under `CONFIG_INFINEON_SMIF_OCTAL=n` by default so the Quad flow is unchanged for everyone except testers.
- **PR 2** (hal_infineon west module, upstream to https://github.com/Infineon/zephyr-ifx-cycfg or whichever repo owns `zephyr-ifx-cycfg`): add the S28HS01GT memslot block in a new file (`cycfg_qspi_memslot_octal.c`) guarded by `CONFIG_INFINEON_SMIF_OCTAL`. Update the west manifest's hal_infineon revision in Zephyr PR 1 to pick up PR 2.
- **Possible PR 3** (MTB template repo): add a `PSE84_SMIF_OCTAL.FLM` file if Infineon produces one and is willing to distribute it under the existing EULA.
- **No PR 4** for this initial enablement — MCUboot OTA stays out of scope until the base path is working. That would be a follow-up PR.

## 10. Infineon-maintainer asks

Questions I cannot answer from the open tree, that need to go to Infineon's Zephyr maintainer channel:

1. **ROM / SE SMIF configuration.** What mechanism configures the Extended Boot ROM's SMIF0 initialization on `KIT_PSE84_EVAL` (eFuse? SE asset table? pre-ROM code in RRAM `extended_boot_reserved`? edgeprotecttools policy?). Which of these can we set from a Zephyr-friendly tool, and which are write-once / provisioning-only?
2. **ROM behavior with Octal.** Does the ROM automatically detect and configure Octal NOR on SMIF0 CS0, or does it require the asset-table / policy to pre-describe the flash (vendor, width, dummy cycles, DDR mode)? If the latter, what is the input format and how is it applied on a development kit?
3. **`PSE84_SMIF.FLM` capability.** Does the existing CMSIS flashloader in `modules/hal/infineon/mtb-template-pse8xxgp/files/flashloader/PSE84_SMIF.FLM` support the 128 MB S28HS01GT on CS0, or does it only know about the 16 MB S25FS128S on CS1? If only Quad, is there a pre-built Octal variant available, or do we need to regenerate it from the QSPI Configurator?
4. **Recommended S28HS01GT memslot config.** Can Infineon share a reviewed `cycfg_qspi_memslot.c` block for S28HS01GTGZBHI030 at SMIF0 CS0 on PSE84, running Octal DDR at 200 MHz with `CY_SMIF_SEL_XSPI_HYPERBUS_WITH_DQS`? Any known DLL tap values for this board?
5. **Coexistence with the Quad on the same data lanes.** The KIT_PSE84_EVAL user guide notes DQ0..3 are shared. Is there any hardware guardband required (pull-ups, isolation resistors, bus-turnaround dwell) or is it purely a matter of CS exclusivity?
6. **MTB SmartwatchDemo Octal variant.** Does any in-house MTB demo boot from the 128 MB Octal on this dev kit? If so, the `design.cyqspi` and `PSE84_SMIF.FLM` from that demo would be the fastest way to unblock this work. If not, why — is the Octal path not yet productized on this silicon revision?
7. **OpenOCD target cfg for pse84.** The current `boards/infineon/kit_pse84_eval/support/openocd.cfg` sources `target/infineon/pse84xgxs2.cfg` which is not shipped with homebrew OpenOCD. What is the upstream source for that target file and can it be added to the Zephyr tree or to an Infineon-maintained OpenOCD fork?
8. **Licensing.** The `.FLM` and `.out` flashloader artifacts are binary blobs under the Infineon EULA. Is the existing practice (bundled in `mtb-template-pse8xxgp/files/flashloader/`) acceptable for an Octal variant too?

---

## Critical Files for Implementation

- /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject/zephyr/drivers/flash/flash_infineon_serial_memory_qspi.c
- /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject/zephyr/boards/infineon/kit_pse84_eval/kit_pse84_eval_memory_map.dtsi
- /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject/zephyr/dts/bindings/flash_controller/infineon,qspi-flash.yaml
- /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject/modules/hal/infineon/zephyr-ifx-cycfg/kit_pse84_eval/cycfg_qspi_memslot.c
- /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject/zephyr/boards/infineon/kit_pse84_eval/support/qspi_config.cfg
