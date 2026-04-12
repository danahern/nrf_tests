# PSE84 Octal Flash Enablement Workspace

Local workspace for enabling the Infineon S28HS01GTGZBHI030 (1 Gb / 128 MB)
SEMPER Octal NOR flash on SMIF0 CS0 of the KIT_PSE84_EVAL board.

See `../PSE84_OCTAL_FLASH_ENABLEMENT_PLAN.md` for the full upstream plan.

## What's in this directory

### `design_src/design.cyqspi`
Input configuration for the ModusToolbox QSPI Configurator. Modified from
the stock TARGET_KIT_PSE84_EVAL_EPC2 BSP to set Slot 0 =
`S28HS01GT (Octal DDR Hybrid at Bottom 25 MHz)` and disable Slot 1
(the 16 MB Quad S25FS128S).

Slot 0 is limited to 64 MB at `0x60000000..0x63FFFFFF` because the SMIF0
XIP aperture is 64 MB wide per range — the second 64 MB of the 128 MB
octal chip would map into a second aperture at `0x64000000..0x67FFFFFF`
via a separate slot if needed. For the animation demos, 64 MB is plenty.

### `cycfg_octal_generated/`
Output of the QSPI Configurator CLI with Slot 0 set to the octal config:

- `cycfg_qspi_memslot.c` / `.h` — the memslot, device, and command structs
  that the Cypress PDL needs to operate the flash in OPI DDR mode.
  All the Infineon-proprietary values (read cmd 0xEE with 20 dummy cycles,
  2-byte OPI command presence, DLL tap, octal-enable register sequence at
  address 0x00800006 with mask 0x01) were emitted by the tool from its
  memory database — we did not hand-write these.
- `qspi_config.cfg` — the openocd SMIF bank definition:
  `0 {addr 0x60000000 size 0x4000000 psize 0x100 esize 0x40000}`
  Bank 0, 64 MB, 256-byte program page, 256 KB erase sector.
- `PSE84_SMIF.FLM` — the **patched** 488 KB CMSIS flashloader that openocd
  loads and runs on the M33 to program the flash. The QSPI Configurator's
  `--flashloader-dir` feature embeds the new memslot struct into the FLM,
  so it knows how to initialize SMIF for OPI DDR before programming.
  (Same file size as the stock FLM, different MD5.)

## How to regenerate

Requires ModusToolbox 3.7 installed at `/Applications/ModusToolbox/`.

```bash
cd /tmp && mkdir -p octal_cfg/flm_src && \
cp /Users/danahern/code/claude/embedded/zephyr_workspace/pse84_octal_enablement/design_src/design.cyqspi octal_cfg/ && \
cp /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject/modules/hal/infineon/mtb-template-pse8xxgp/files/flashloader/PSE84_SMIF.FLM octal_cfg/flm_src/

/Applications/ModusToolbox/tools_3.7/qspi-configurator/qspi-configurator-cli \
    -c /tmp/octal_cfg/design.cyqspi \
    -o /tmp/octal_cfg/out \
    -f /tmp/octal_cfg/flm_src
```

## Next steps (not done yet)

1. Drop `cycfg_qspi_memslot.c/.h` into the Zephyr build so it overrides the
   stock Quad version from `modules/hal/infineon/zephyr-ifx-cycfg/kit_pse84_eval/`.
2. Add Zephyr SoC-level early init that calls `Cy_SMIF_Init()` +
   `Cy_SMIF_MemInit()` to transition SMIF from the ROM-default Quad mode
   into Octal DDR mode. This code MUST run from SRAM (not XIP) because the
   transition bus-faults any live XIP reads from SMIF0.
3. Gate all of the above behind a new `CONFIG_INFINEON_SMIF_OCTAL` Kconfig
   symbol that defaults `n`, so the existing Quad boot path is unchanged
   for any board that doesn't explicitly opt in.
4. Update the KIT_PSE84_EVAL board memory map to reference
   `<MemoryId>S28HS01GT (Octal DDR Hybrid at Bottom 25 MHz)</MemoryId>`
   and resize partitions per AN237857 Figure 53 recommendations.
5. Wire a new openocd config variant that uses the patched FLM at
   bank 0 rather than the stock quad bank at bank 1.
6. Test boot-from-octal end-to-end on hardware; if SMIF transition is
   failing, investigate whether the transition code needs `__ramfunc` or
   a similar linker section placement.

## Risks / open questions

- The transition from ROM-default Quad to runtime Octal while live-XIP is
  the single biggest unknown. Worst case: bus-faults during the SMIF
  reconfigure, requires placing the reconfigure code and a trampoline in
  SRAM (ITCM/DTCM on M33), plus a vector-table relocation. Non-trivial.
- `PSE84_SMIF.FLM` has been patched in place by the Configurator. I have
  not yet verified whether the patched FLM actually programs the octal
  chip correctly — that requires hardware testing with `west flash`.
- This commits moderately-large Infineon-generated binary artifacts
  (488 KB FLM) into the tree. Track via git-lfs if the outer repo's
  size budget is tight.
