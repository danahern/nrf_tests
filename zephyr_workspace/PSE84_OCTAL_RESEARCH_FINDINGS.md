# PSE84 Octal Flash Research Findings

Addendum to `PSE84_OCTAL_FLASH_ENABLEMENT_PLAN.md`. Captures what web research
and the Infineon tooling on disk answered that I had to flag as "needs Infineon
input" in the original plan.

Date: 2026-04-11.

## Questions closed

### Plan §3 / §10.1 — "How does the Extended Boot ROM's SMIF0 get configured?"

**Answer: it doesn't matter — the M33 secure image reconfigures SMIF itself after the ROM hands off.**

Evidence:
- **AN237857 §1.1 boot flow diagram:** Secure Enclave → Extended Boot → Edge Protect Bootloader → CM33_S → CM33_NS → CM55. Extended Boot is "owned by Infineon" and its only job is to verify and launch the first M33S image. There is no SMIF-specific configuration step listed for Extended Boot.
- **TARGET_KIT_PSE84_EVAL_EPC2 BSP `cybsp.c`:** the standard application init path on M33S calls `init_cycfg_all()`, which is a generated function from the Device Configurator. That function — when the QSPI Configurator has declared an octal memslot — calls `Cy_SMIF_Init()` + `Cy_SMIF_MemInit()` and transitions SMIF from whatever the ROM left it in over to the application's desired configuration.
- **SmartwatchDemo `cycfg_peripherals.c::init_cycfg_peripherals()`:** only calls `Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SMIF0_...)` — no `Cy_SMIF_Init()`. This is because SmartwatchDemo stays on the ROM-default Quad config; applications that want a different SMIF state are the ones that run the transition.
- **`Infineon/ifx-mcuboot-pse84` README.md §"OCTAL memory support on PSE84 EVK":** the entire recipe is a GUI-level config change in the Device Configurator + QSPI Configurator. No mention of eFuses, SE asset tables, provisioning flows, or re-provisioning the device. If ROM-level config were write-once the docs would say so.

**Implication:** The M33 secure image is responsible for the SMIF mode transition. This is pure application code, no access to privileged provisioning interfaces needed. The one catch — the transition code has to run from SRAM, not XIP — is a well-understood problem with standard mitigations (`__ramfunc`-style linker section, trampoline jump after the transition).

### Plan §10.3 — "Does the existing `PSE84_SMIF.FLM` support the octal S28HS01GT?"

**Answer: no, but the QSPI Configurator can patch it in place.**

Evidence:
- The stock `PSE84_SMIF.FLM` in `mtb-template-pse8xxgp/files/flashloader/` is 488,648 bytes and was built with the `S25FS128S (Hybrid at bottom, 64 KB sectors elsewhere)` memory on Slot 1. It has no built-in knowledge of the S28HS01GT.
- The `qspi-configurator-cli --flashloader-dir <dir>` option is designed to **patch an existing FLM in place** with the memslot configuration from the design.cyqspi. The CLI docs describe it as "place a patched copy of in output-dir".
- I verified this works empirically on this machine: running the CLI with `-f /tmp/octal_cfg/flm_src` against a modified design.cyqspi produced a new `PSE84_SMIF.FLM` of the same 488,648-byte size but a different MD5 (`0f477c3dcc1311c36a22874538fa0458` vs original `135196ed2bfa920982d0755016b04e0a`). The memslot struct is embedded in the FLM binary at a known offset the CLI knows how to find.

**Implication:** No FLM rebuild required. We take the stock `PSE84_SMIF.FLM`, pass it through the Configurator CLI once with our octal design.cyqspi, get a patched FLM, and drop that into `boards/infineon/kit_pse84_eval/support/`.

### Plan §10.4 — "What's the reviewed S28HS01GT memslot config?"

**Answer: the QSPI Configurator shipped in ModusToolbox ships a complete pre-canned preset.**

Evidence from this machine's ModusToolbox install (3.7.0):
- `/Applications/ModusToolbox/tools_3.7/qspi-configurator/qspi-configurator.app/Contents/data/memory/S28HS01GT-hybridbottom.cymem` — base memory descriptor with page size 256 B, erase block 256 KB, JEDEC-verified hybrid sector layout (32× 4KB sectors at bottom, 1× 32KB, then 511× 256KB main), datasheet-derived base commands.
- The "Octal DDR Hybrid at Bottom 25 MHz" label in the UI is a preset that extends this base with OPI DDR command variants (2-byte OPI commands, DDR rates, 20 dummy cycles, etc.). That preset is hardcoded inside the `qspi-configurator` binary — I verified the string "Octal DDR" exists in the binary but not in any external data file.
- I ran the CLI with a modified design.cyqspi that requests this preset on Slot 0, and the generator produced a complete `cycfg_qspi_memslot.c` with:
  - `readCmd`: `.command = 0xEE`, `.commandH = 0xEE`, `.cmdPresence = CY_SMIF_PRESENT_2BYTE`, all widths `CY_SMIF_WIDTH_OCTAL`, all rates `CY_SMIF_DDR`, `.dummyCycles = 20`
  - `writeEnCmd`: 2-byte `0x06`, octal, DDR
  - `eraseCmd`: 4-byte addressing variant
  - `deviceCfg`: `.octalEnableRegAddr = 0x00800006`, `.stsRegOctalEnableMask = 0x01`, `.freq_of_operation = CY_SMIF_100MHZ_OPERATION`, octal-enable sequence block
  - `cy_stc_smif_mem_config_t`: `.slaveSelect = CY_SMIF_SLAVE_SELECT_0`, `.dataSelect = CY_SMIF_DATA_SEL0`, `.baseAddress = 0x60000000`, `.memMappedSize = 0x04000000` (64 MB), `.flags = CY_SMIF_FLAG_SMIF_REV_3 | CY_SMIF_FLAG_MEMORY_MAPPED | CY_SMIF_FLAG_WR_EN | CY_SMIF_FLAG_MERGE_ENABLE`, `.mergeTimeout = CY_SMIF_MERGE_TIMEOUT_256_CYCLES`

Full output saved to `pse84_octal_enablement/cycfg_octal_generated/`.

**Implication:** The plan's largest risk — "hand-written OPI commands will be wrong on first try" — is eliminated. We use the Configurator output verbatim.

### Plan §10.6 — "Does any MTB example boot from octal on this dev kit?"

**Answer: yes — the Edge Protect Bootloader example at `Infineon/ifx-mcuboot-pse84`, following the recipe in its README §"OCTAL memory support on PSE84 EVK".**

Evidence:
- The repo has a `boot/platforms/COMPONENT_PSE84/COMPONENT_MCUBOOT_SMIF_FLASH_MEM/` directory containing `mcuboot_smif_config.h` and `external_memory.c` that wire `smif0BlockConfig.memConfig[0]` to MCUboot's external flash abstraction.
- The README walks through enabling the octal flash via QSPI Configurator, then points at the generated config. The bootloader itself doesn't hardcode anything flash-specific.
- The Device Configurator linker script references memory regions at the octal addresses (`m33s_nvm` at 0x00100000 offset, `m33_nvm` at 0x00340000, `m55_nvm` at 0x00580000 — matching AN237857 Figure 53 exactly).

**Implication:** There is a production-quality reference implementation we can study when implementing the Zephyr side. Even if we don't use MCUboot for Zephyr, we can read the EPB's SMIF init sequence to understand exactly how the Quad-to-Octal transition is performed in practice.

## Questions still open

### Plan §10.5 — "DQ0..3 shared between Quad and Octal — any hardware guardband?"

Still unclear from open docs. The KIT_PSE84_EVAL user guide §3.2.1.2.1 states:
> By default, Smif0_Select1 is connected to this interface. IO lines are shared between the Octal and Quad flash in this board.

The recipe in AN237857 and the ifx-mcuboot-pse84 README both commit the entire boot chain to one flash or the other with no mention of coexistence, which strongly implies: **don't try to use both simultaneously**. The chip-select alone is sufficient to differentiate, but only one memslot should be `MemoryMapped = true` at a time. The QSPI Configurator enforces this by making you explicitly "Not used" the old slot.

### Plan §10.7 — "Upstream source for `target/infineon/pse84xgxs2.cfg` openocd target"

Unanswered. The Zephyr board's `openocd.cfg` sources `target/infineon/pse84xgxs2.cfg` which is not in homebrew openocd, so users are relying on the Infineon-patched openocd shipped with `ModusToolboxProgtools-1.8`. This is also true for the existing Quad setup — it's not unique to octal.

For the octal path, the `qspi_config.cfg` generated by the Configurator gets sourced by the same pse84xgxs2.cfg, so no new openocd-tree changes are needed.

### Plan §10.8 — "FLM licensing for upstream distribution"

Unanswered. The generated `PSE84_SMIF.FLM` is an Infineon binary with EULA-restricted distribution. Current Zephyr practice is to bundle the stock quad FLM inside `modules/hal_infineon/mtb-template-pse8xxgp/files/flashloader/` — this suggests Infineon already permits bundling, but I don't have an explicit confirmation that the octal variant is covered.

For local use (not upstream), this is a non-issue — we can use the patched FLM directly.

## New questions that arose from the research

### R1. How does AN237857's recipe handle the SMIF mode transition without bus-faulting the running code?

The app note §6 says "Configure memory regions" and "Update slot information in the bootloader solution" — but doesn't describe the code-level sequence. The BSP's `init_cycfg_all()` must be calling `Cy_SMIF_Init()` from a safe context somehow.

Likely answer: the BSP places `init_cycfg_peripherals()` or the transition sub-function in a `__ramfunc`-tagged section that the linker puts in SRAM. Needs verification by looking at the generated `cycfg_peripherals.c` for an octal-configured build (which I don't have — my tools only generate `cycfg_qspi_memslot.c`, not the peripherals file, because the peripherals file comes from the Device Configurator not the QSPI Configurator).

**Action:** before attempting the transition in Zephyr, either:
- Install the MTB Device Configurator, run it against the ifx-mcuboot-pse84 example with octal enabled, get the generated `cycfg_peripherals.c`, and study its Cy_SMIF_Init call site, OR
- Hand-implement the transition using the Cypress PDL `Cy_SMIF_Init()` / `Cy_SMIF_MemInit()` API directly, placing our init function in `.itcm` or a custom `__ramfunc` section.

### R2. Will the existing Zephyr M33 image (linked for quad-mode m33s_xip at `0x60100400`) boot correctly if the ROM thinks it's in octal mode?

Unknown. Technically the bytes at `0x60100400` are the same regardless of SMIF mode (physical flash contents). Practically, the signed MCUboot header at `0x60100000` that the ROM verifies must be programmed correctly in both octal and quad workflows. If the octal FLM programs bytes at the correct physical addresses, the ROM shouldn't care.

**Action:** verify by programming the existing Zephyr quad M33 image to the octal flash via the patched FLM, then checking whether the ROM accepts it. If yes, the two configs are interchangeable at the bytes-on-flash level and only the SMIF controller state differs.

## Practical next-steps checklist

1. ☐ Investigate the R1 question — get a working `cycfg_peripherals.c` from a MTB octal build, or accept that I'll hand-write the transition
2. ☐ Drop `cycfg_octal_generated/cycfg_qspi_memslot.c` into `modules/hal/infineon/zephyr-ifx-cycfg/kit_pse84_eval/` in a Kconfig-gated form
3. ☐ Write a SoC early-init hook that calls `Cy_SMIF_Init()` + `Cy_SMIF_MemInit()` from `.ramfunc`
4. ☐ Add `CONFIG_INFINEON_SMIF_OCTAL` Kconfig symbol defaulting `n`
5. ☐ Build a test image with `CONFIG_INFINEON_SMIF_OCTAL=y` but still linked at the current Quad m33s_xip addresses — verify it boots
6. ☐ Try flashing the test image with the patched FLM and a modified `qspi_config.cfg`
7. ☐ If the above works, update the board memory map to the 64 MB octal layout and re-test
8. ☐ If the above fails, add runtime debug prints from the transition code (running in SRAM, before the SMIF switch, so they should work)

## References

- AN237857 Edge Protect Bootloader for PSOC Edge MCU: https://www.infineon.com/assets/row/public/documents/30/42/infineon-an237857-edge-protect-bootloader-psoc-edge-mcu-applicationnotes-en.pdf
- AN237849 Getting started with PSOC Edge security: https://www.infineon.com/assets/row/public/documents/30/42/infineon-an237849-getting-started-psoc-edge-security-applicationnotes-en.pdf
- S28HS01GT SEMPER Octal NOR datasheet: https://www.infineon.com/dgdl/Infineon-S28HS512T_S28HS01GT_S28HL512T_S28HL01GT_512MB_1GB_SEMPER_TM_FLASH_OCTAL_INTERFACE_1_8V_3-DataSheet-v68_00-EN.pdf
- KIT_PSE84_EVAL user guide: https://www.infineon.com/assets/row/public/documents/30/44/infineon-kit-pse84-eval-user-guide-usermanual-en.pdf
- ModusToolbox QSPI Configurator user guide: https://www.infineon.com/assets/row/public/documents/30/44/infineon-modustoolbox-qspi-configurator-user-guide-usermanual-en.pdf
- `Infineon/ifx-mcuboot-pse84` GitHub: https://github.com/Infineon/ifx-mcuboot-pse84
- `Infineon/TARGET_KIT_PSE84_EVAL_EPC2` GitHub: https://github.com/Infineon/TARGET_KIT_PSE84_EVAL_EPC2
