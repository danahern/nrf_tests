# PSE84 Octal NOR enablement via extended boot policy — rolling log

**Goal:** Boot directly from the 128 MB S28HS01GT octal NOR on SMIF0/CS0 by updating the OEM extended-boot policy. Extended boot configures SMIF0 at reset (before any user code runs), so Zephyr has no runtime transition to perform. SMIF1/CS2 HyperRAM PSRAM remains initialized by the Zephyr M33 secure companion (already working).

**Board:** KIT_PSE84_EVAL_EPC2, PSE846GPS2DBZC4A, B0 silicon, DEVELOPMENT LCS.

**Reference:** PSE84 architecture ref manual §17.2.4.2.2 "Detection of external flash" and §17.2.4.2.3 "Boot sources". Policy schema in `zephyr_workspace/pse84_provision/policy/policy_oem_provisioning.json` — the fields we care about are `extended_boot_policy.external_flash.smif_chip_select`, `.smif_data_width`, `.smif_data_select`.

---

## Current state (entering this effort)

- ✅ **SMIF1/CS2 HyperRAM PSRAM working.** CM55 XIP round-trip verified at 0x64000000..0x64800000 with canaries. Commit `62f7e55cae9` (zephyr) + `cbda1c3` (outer).
- ✅ **SMIF0/CS1 Quad NOR working by ROM default.** M55 XIPs from 0x60500000 (image) on 16 MB aperture. This is what extended boot currently brings up per the default OEM policy (`smif_chip_select: 1`, `smif_data_width: 4`).
- ❌ **SMIF0/CS0 Octal NOR (128 MB) inaccessible.** Multiple Zephyr-side runtime-transition attempts in this session and prior ones; all either wedge SMIF in `Cy_SMIF_BusyCheck` or bus-fault before yielding a boot. Commit `e114550` documented the flakiness ("works when it works").
- ✅ **Flashloader for CS0 is in place (commit `b6d47af`).** Real patched FLM at `zephyr_workspace/pse84_octal_enablement/cycfg_octal_generated/PSE84_SMIF.FLM` (MD5 `5aedddc9...`) makes `west flash` capable of writing bytes to CS0.
- ❌ **Extended boot policy has NOT been updated to CS0/octal.** This is the missing piece. Policy in `zephyr_workspace/pse84_provision/policy/policy_oem_provisioning.json` still says `smif_chip_select: 1`, `smif_data_width: 4`.

## Approach — Path B

Move SMIF0 octal configuration from Zephyr runtime code into the extended-boot policy (§17.2.4.2.2). Then:

1. Extended boot reads the policy and configures SMIF0 for CS0 octal DDR XiP **before** the CM33 secure image runs.
2. CM33 secure image (our Zephyr companion) runs with SMIF0 already in octal mode — no transition needed. Skips `ifx_pse84_smif_octal_init` entirely.
3. Zephyr's existing `ifx_pse84_psram_init` continues to init SMIF1 PSRAM (that's not covered by extended boot, only SMIF0).
4. Images are flashed to CS0 (via the patched FLM from `b6d47af`), not CS1.

## Work order

- [ ] **1. Audit the existing `pse84_provision` workspace** — understand what was done before, what tooling invocations are logged, current policy/keys state.
- [ ] **2. Clone `policy_oem_provisioning.json` → `policy_oem_octal.json`** with `smif_chip_select: 0`, `smif_data_width: 8`. Review `oem_alt_app_address` for CS0.
- [ ] **3. Dry-run `edgeprotecttools`** to validate the new policy and generate the CBOR.
- [ ] **4. Coordinated flash:** images to CS0 first (with CS1-quad policy still live for recovery), then apply the new policy.
- [ ] **5. Verify extended boot** via `0x34000000` mode code = `0xAA000003` (`IFX_BOOT_MODE_LAUNCH_NEXT_APP`). Test XIP reads across the full 64 MB CS0 XIP aperture.
- [ ] **6. Delete obsolete octal code** from `zephyr/soc/infineon/edge/pse84/security_config/pse84_boot.c` once step 5 is green. Keep `CONFIG_INFINEON_SMIF_OCTAL` as a DT partition-map selector, not a runtime-transition trigger.

## Rolling log

### 2026-04-12 (this session — plan phase)

**Where we started in this session:**
- Multiple attempts at runtime SMIF0 octal transition via both the elaborate teardown (current zephyr tree) and the simpler `mtb_serial_memory_setup` form (original commit `9664687cd26`). Every attempt wedges SMIF in `Cy_SMIF_BusyCheck`, leaving the chip SWD-unresponsive and requiring a physical power cycle to recover. Burned ~5 power cycles.
- Read PSE84 arch ref manual §17.2.4 "Boot sequence" and established the policy-driven path above.
- Confirmed policy JSON has exactly the knobs needed — `smif_chip_select`, `smif_data_width`, `smif_data_select`.
- Confirmed tooling: `/opt/homebrew/bin/edgeprotecttools` present; `zephyr_workspace/pse84_provision/` already contains a policy file, keys, packets, and logs from `2026-04-06`.

**Key learning:** commit `b6d47af` addressed the *write-time* problem (getting bytes into the octal chip during flashing) but did NOT address the *boot-time* problem (getting extended boot to bring SMIF0 up on CS0 with octal protocol). That's the gap this effort closes.

**Next:** audit the `pse84_provision` workspace to understand prior provisioning history and tool invocation shape.

### 2026-04-12 — audit of pse84_provision workspace (step 1 of work order) ✅

**Prior provisioning history:** A full OEM provisioning completed successfully on **2026-04-06 22:55:30** — logs end with `PROVISIONING PASSED`. The policy applied was `policy/policy_oem_provisioning.json` as it currently stands (i.e., `smif_chip_select: 1`, `smif_data_width: 4` — default CS1 quad). So the current on-device extended-boot policy matches what's in the repo.

**Working tool invocation (captured from log `2026-04-06_22-55-25`):**
```
/opt/homebrew/bin/edgeprotecttools -t pse8xs2 provision-device \
  -p policy/policy_oem_provisioning.json \
  --key keys/oem_rot_priv_0.pem \
  --ifx-oem-cert packets/oem_cert_signed.bin
```

Under the hood: edgeprotecttools signs the policy with the OEM RoT key, packs it with IFX-signed OEM cert into a DLM (download manager) package (`cyapp_prov_oem_dlm.hex`), loads it into CM33 SRAM at `0x34008000`, triggers the ROM boot loader to validate + execute it (status at `0x5240040c` returns `0xf2a00001` = `CYAPP_SUCCESS`), then resets the device. Done.

**Status read from BOOTROW `0x52400420..0x5240042c`** returned `0x000000d780000b76bc0000db 00000000` = **DEVELOPMENT LCS**. Matches what the openocd acquire banner shows today. Re-provisioning in DEV LCS is permitted by Infineon's lifecycle rules (§17.2.2.2 — "In this LCS, the device can be provisioned, new policies can be deployed").

**Existing assets we don't have to recreate:**
- `keys/oem_rot_priv_0.pem` / `oem_rot_pub_0.pem` — OEM RoT key pair
- `packets/oem_cert_signed.bin` — IFX-signed OEM cert (signs the chain of trust to the OEM RoT key)
- `packets/apps/prov_oem/cyapp_prov_oem_signed_*.bin` — the IFX-signed provisioning app binary
- `packets/apps/extended_boot/extended-boot-{serial,no-serial}.cbor` — signed extended boot images
- `.edgeprotecttools` — tool settings pinning openocd to MTB at `/Applications/ModusToolboxProgtools-1.7/openocd`

**Key insight for Path B:** Re-running the exact same provision-device command with a modified policy JSON (just the three `external_flash` fields changed) should update the on-device extended-boot policy to CS0 octal. No regeneration of certs, keys, or apps needed — the tool re-signs the policy each invocation.

**Caveat:** `prov_oem`'s `config.json` says `allowed_lcs: ["NORMAL_PROVISIONED", "SECURE"]`, not `DEVELOPMENT`. But the 2026-04-06 run succeeded from DEVELOPMENT LCS (per BOOTROW reading before + after), so either the tool's internal check differs from the config.json metadata, or DEV is implicitly allowed. Will confirm empirically on re-run.

**Next:** step 2 — clone `policy_oem_provisioning.json` → `policy_oem_octal.json` with the three field changes, dry-run to validate the new policy.

### 2026-04-12 — step 2 + 3: octal policy cloned, offline-validated ✅

Created `zephyr_workspace/pse84_provision/policy/policy_oem_octal.json` as a byte-identical copy of `policy_oem_provisioning.json` except for two fields:

```diff
-  "smif_chip_select": { ..., "value": 1 },   # CS1 (quad)
+  "smif_chip_select": { ..., "value": 0 },   # CS0 (octal)

-  "smif_data_width":  { ..., "value": 4 },   # quad
+  "smif_data_width":  { ..., "value": 8 },   # octal
```

`smif_data_select: 0` is kept (0 = d0..d7 on standard SMIF0 pins).

**Offline validation (no device touch):**
```
edgeprotecttools -t pse8xs2 create-provisioning-packet \
  -p policy/policy_oem_octal.json \
  --key keys/oem_rot_priv_0.pem \
  --ifx-oem-cert packets/oem_cert_signed.bin
```
→ `Signed packet ... Provisioning packet created: packets/apps/prov_oem/in_params.bin`
→ `edgeprotecttools verify-packet --packet in_params.bin --key oem_rot_pub_0.pem` → `Signature verified`.
→ Policy validates against `policy_prov_oem_schema.json_schema`. No errors.

**Non-trivial caveat I uncovered while reading §17.2.4.2.2:**

The arch ref Figure 80 flowchart says: *"The extended boot powers on and configures the external flash **only when the application launch address is located within the external flash**."* — and our current policy has `oem_app_address: 0x32011000` (in RRAM), not external flash. Yet CS1 quad XIP clearly works today for M55. Two possibilities I can't yet resolve from the docs alone:

  1. The default policy's "auto SFDP mode" implicitly turns on external flash regardless of main app location, because `oem_alt_boot` is true and `oem_alt_app_address: 0x70100000` IS in external flash. So extended boot considers external flash reachable via the alt-boot path and configures it proactively.
  2. Or: extended boot configures SMIF0 lazily on first fetch when M55 is enabled (unlikely, since the hardware needs device slot registers programmed before XIP works at all).

Empirical evidence from our current system favors (1) — the `enable_alt_serial: true` + `oem_alt_boot: true` in policy, plus SMIF0/SS1 quad working, means extended boot is configuring SMIF0 even with main app in RRAM.

**Implication for Path B:** Changing `smif_chip_select` from 1→0 in the policy should make extended boot configure SMIF0 for **CS0 octal** instead of CS1 quad, at next reset. The M33 secure companion continues to load from RRAM — no change there.

### Coordination problem: M55 image location

M55 is not a bootloader stage — it's the main application that XIPs from external flash at whatever address the linker places its image. Today that's `m55_xip` partition at `0x60500000` (ROM default quad layout).

After policy switch to CS0 octal:
- SMIF0 XIP is now mapped to CS0 at 0x6000_0000.
- `0x60500000` now reads from the **S28HS01GT octal chip**, not the S25FS128S quad chip.
- The octal chip currently has **nothing meaningful** at offset 0x500000 — `west flash` has only ever written the M55 image to the quad chip CS1.
- M55 vector[0] would be 0xFFFFFFFF → invalid SP → M55 crash immediately.

**Therefore step 4 (the "coordinated flash") must be:**

  4a. Add `kit_pse84_eval_octal_64mb.overlay` to the test app so the linker places the M55 image in the octal partition layout (m55_xip @ 0x60580000 per commit `5a7501e57a4`).
  4b. Rebuild the app. M55 image is now built for 0x60580000 base.
  4c. Two-pass flash using the **patched octal FLM** (`SMIF_OCTAL_FLM=...`) so both M33 signed hex AND M55 hex get written to CS0 at their new offsets. **Critical:** verify that pass 2 actually writes the M55 hex, not just M33 — prior session logs showed pass 2 only writing 69 KB (M33 only). May need `west flash` both domains explicitly, or a different invocation.
  4d. THEN `edgeprotecttools reprovision-device -p policy_oem_octal.json ...` to apply the new policy.
  4e. Reset. Extended boot sees octal policy, configures SMIF0 for CS0 octal XIP, jumps to M33 RAM app, which enables M55, M55 XIPs cleanly from CS0 @ 0x60580000.

If any step fails, recovery path: P17.6 high triggers `oem_alt_boot` → boots from 0x70100000 (secure alias of 0x60100000, same addr — which is still CS0 now). If CS0 has no valid image, we'd still be stuck. **Safer recovery:** re-provision with original CS1/quad policy to restore ROM-default behavior. We need the `policy_oem_provisioning.json` untouched as the "rollback" file.

### Open question for the user

Before proceeding to step 4 on hardware, one clarification: do you want me to also update **`oem_alt_app_address`** (currently `0x70100000`) for the octal layout? It's the alt-boot target used when P17.6 is high. With octal active, `0x70100000` maps to CS0 at offset 0x100000 (same as `0x60100000` NS — the M33 signed hex default location). Under the octal partition overlay, the M33 signed hex moves too — to offset 0x100400 ("m33s_xip" 2559 KB starting at 0x60100400 per overlay commit message).

I'll leave `oem_alt_app_address: 0x70100000` for now and only adjust if we hit an alt-boot test case.

**Next:** step 4a — add octal overlay to pse84_psram_test, rebuild, verify M55 image link address in the resulting elf.

### 2026-04-12 — step 4 executed end-to-end: **SUCCESS** ✅

Deviation from earlier plan: we did NOT add `kit_pse84_eval_octal_64mb.overlay`. The M55 image stayed linked at the quad-layout address `0x60500000`. That maps to CS0 offset `0x500000` — well within the 128 MB octal chip. We'll add the fuller partition overlay later if we want the larger m55_xip partition; for the boot-proof we just needed the image to exist on CS0 at whatever offset the linker picks.

**4a. Build:** clean rebuild with OCTAL=y on both M55 prj.conf and M33 enable_cm55.conf. Initially tried OCTAL=n on M33 to avoid the runtime transition. Build fine, M55 ELF sections place rom_start at 0x60500000 as expected (`arm-zephyr-eabi-objdump -h` confirms).

**4b. Four-pass flash:**
  - Pass 1 (stock FLM, CS1 quad, safety): `west flash --domain enable_cm55` + `--domain pse84_psram_test`. Both wrote successfully (~69 KB M33 signed + ~61 KB M55 hex).
  - Pass 2 (patched FLM, CS0 octal): `SMIF_OCTAL=1 SMIF_OCTAL_FLM=.../PSE84_SMIF.FLM west flash --domain enable_cm55` + `--domain pse84_psram_test`. Both wrote successfully.
  - **Both images now exist on both chips.** Policy switch is the only thing that changes which chip is active.

**4c. Apply octal policy — first attempts failed, learned two things:**
  1. `reprovision-device` rejected the policy: `"The extended boot image updating is not allowed during reprovisioning. Remove the 'extended_boot_image' property from the policy"`. **Fix:** stripped the `extended_boot_image` block from `policy_oem_octal.json`. That property is only used by `provision-device` (initial assets install), not `reprovision-device` (policy-only updates).
  2. `reprovision-device` then rejected with: `"Reprovisioning is only available in the PRODUCTION LCS"`. We're in DEVELOPMENT LCS. **Fix:** use `provision-device` instead, which works in DEV LCS and repeatedly accepts policy changes per §17.2.2.2.
  - **Working invocation:**
    ```
    edgeprotecttools -t pse8xs2 provision-device \
      -p policy/policy_oem_octal.json \
      --key keys/oem_rot_priv_0.pem \
      --ifx-oem-cert packets/oem_cert_signed.bin
    ```
  - Result: `PROVISIONING PASSED`, chip stays in DEV LCS, new policy live.

**4d. First reset — partial success:**
  - Board booted fine, M55 printed the probe output, PSRAM worked.
  - But `+16 MB @0x61000000` BUS FAULT — exactly as before.
  - **Not a SMIF issue** — the bus fault was coming from the M55 MPC. `pse84_s_protection.c:160` caps the M55 SMIF0 region size at 11 MB (`0x00B00000`) when `CONFIG_INFINEON_SMIF_OCTAL` is **off** on the M33 companion (which I'd set earlier, thinking OCTAL was purely a runtime-transition flag). With OCTAL off, the region is 11 MB (quad chip size). Reads past 0x61000000 get denied by MPC, not by SMIF hardware.
  - **Key learning:** `CONFIG_INFINEON_SMIF_OCTAL` on the M33 companion is NOT just a runtime-transition flag. It also gates: (1) the 58 MB M55 MPC region size on SMIF0, (2) the `smif0BlockConfig` source file selection (octal vs quad cycfg). Independent of whether we call the runtime transition function.

**Fix:** re-enable `CONFIG_INFINEON_SMIF_OCTAL=y` on the M33 companion AND remove the call to `ifx_pse84_smif_octal_init()` in `pse84_boot.c` (extended boot does the transition now). Rebuild + reflash M33+M55 to CS0.

**4e. Second reset — FULL SUCCESS:**
```
== smif0+smif1 combined probe ==
-- SMIF1 PSRAM read/write --
post @0x64000000 = 0xcafebabe (want CAFEBABE)  ← PSRAM still works
post @0x64010000 = 0xdeadbeef (want DEADBEEF)
post @0x64800000 = 0x12345678 (want 12345678)
-- SMIF0 Octal NOR read (XIP, 64 MB aperture) --
M55 img  @0x60500000 = 0x26103568 (vector[0] = initial SP)
+8 MB    @0x60800000 = 0xffffffff
+16 MB   @0x61000000 = 0xffffffff (proves >16 MB — octal active)
+32 MB   @0x62000000 = 0xffffffff
+60 MB   @0x63C00000 = 0xffffffff (near end of 64 MB XIP)
== probe done ==
```
- ✅ Full 64 MB SMIF0 XIP aperture readable from M55. Proves CS0 octal is live.
- ✅ M55 XIP boot works from CS0 (M55 image was flashed to offset 0x500000 of the octal chip via the patched FLM).
- ✅ PSRAM on SMIF1 continues to work in parallel.
- ✅ No runtime SMIF transition code ran. Extended boot policy did it all.
- ✅ No BusyCheck wedge — the failure mode that dominated all prior sessions is gone.

## Summary — Path B complete

The whole boot flow now:
1. Reset.
2. PSE84 ROM runs, then SE RT Services.
3. **Extended Boot** reads OEM policy, sees `smif_chip_select: 0`, `smif_data_width: 8`. Configures SMIF0 for CS0 in octal DDR XIP mode via SFDP probe. 64 MB XIP aperture at 0x60000000.
4. Extended boot jumps to `oem_app_address: 0x32011000` in RRAM → the Zephyr M33 signed companion.
5. M33 companion (`ifx_pse84_cm55_startup`) runs normally — no SMIF0 transition needed. Still runs `ifx_pse84_psram_init` for SMIF1 HyperRAM PSRAM.
6. `Cy_SysEnableCM55` releases M55.
7. M55 XIPs from its image at 0x60500000 on CS0 octal, prints the probe output, reads SMIF1 PSRAM with canary round-trip, reads the full 64 MB SMIF0 XIP aperture.

**Things to commit:**
- `zephyr/soc/infineon/edge/pse84/security_config/pse84_boot.c` — removed the `ifx_pse84_smif_octal_init()` call at the top of `ifx_pse84_cm55_startup`; extended doc comment explaining why.
- `zephyr_workspace/pse84_psram_test/prj.conf` — OCTAL=y (for M55 MPC + cycfg).
- `zephyr_workspace/pse84_psram_test/sysbuild/enable_cm55.conf` — OCTAL=y with explanatory comment.
- `zephyr_workspace/pse84_psram_test/src/main.c` — octal probe up to +60 MB.
- `zephyr_workspace/pse84_provision/policy/policy_oem_octal.json` — new policy file (CS0, 8-bit, no `extended_boot_image`).
- `docs/pse84_octal_policy_enablement.md` — this rolling log.

## Follow-ups (not required for the primary goal)

- Add a Kconfig `CONFIG_INFINEON_SMIF0_EXT_BOOT_CONFIGURED` (bool, default y if OCTAL) that semantically marks "extended boot already did the transition, skip runtime init" — currently implicit, that knob would make the intent explicit.
- Consider adding the `kit_pse84_eval_octal_64mb.overlay` to the test app to access the full 64 MB via a real partition table. Not needed for boot, but needed for app data / OTA / MCUboot slots.
- Upper 64 MB of the 128 MB chip (0x64000000–0x67FFFFFF in the device address space, but that overlaps PSRAM!) needs MMIO access, not XIP, because one SMIF XIP port is capped at 64 MB. Defer.
- The stale `ifx_pse84_smif_octal_init` static function in `pse84_boot.c` is now unreferenced. Kept for potential future re-use with a "RUNTIME_INIT" Kconfig. Could delete if that cleanup matters.
- The `policy_oem_provisioning.json` (original CS1/quad) is our rollback policy — keep it in the repo.
