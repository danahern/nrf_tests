# PSE84 I2C Debug — Status 2026-04-09 (End of Session)

## Current State
- Board stuck in HardFault loop (PC=0xeffffffe, XPSR=0x89000003)
- DPLL_LP0 is actually running (0x42403200 = 0x8ec00413) from a previous boot
- CLK_ROOT10 = 0x80000100 (ClkPath0/2 = 200 MHz now, was 100 MHz in demo)
- **Requires physical USB power cycle to recover**

## Key Discoveries Today

### 1. HSIOM 0x0F is CORRECT for I2C on PSE84 BGA-220
From BSP `gpio_pse84_bga_220.h`: `P8_0_SCB0_I2C_SCL = 15` (0x0F). Earlier assumption of 0x0B was wrong.

### 2. Zephyr clock driver supports DPLL via DTS
`zephyr/drivers/clock_control/clock_control_infineon_fixed_clock.c` handles DPLL_LP0 configuration via DT properties:
```
&dpll_lp0 {
    status = "okay";
    clock-frequency = <400000000>;
    feedback-div = <69>;
    reference-div = <3>;
    output-div = <1>;
    dco-mode-enable;
    fraction-div = <12657810>;
};
```

### 3. DPLL Register Location
DPLL_LP0 registers are at **0x42403200** (NOT 0x42400600 as I initially tried)
- 0x42403200: CONFIG (bit 31 = enabled)
- 0x42403218: TEST3 (Zephyr workaround writes this)
- 0x4240321C: TEST4 (Zephyr workaround writes this)

### 4. Demo Clock Tree Values
From ModusToolbox Device Configurator (smartwatch demo):
- DPLL_LP0: 400 MHz (Feedback=69, Reference=3, Output=1, DCO=true, Frac=12657810)
- CLK_HF10: ClkPath0/4 = 100 MHz → PERI0 Group1 → SCB0
- CLK_HF12: 24 MHz (GFXSS)
- CLK_HF1: 400 MHz (CM55)
- CLK_HF0: 200 MHz (CM33)

### 5. I2C Register Values (match demo)
- SCB0 CTRL: 0x80000000 (enabled, OVS=0)
- SCB0 I2C_CTRL: 0x800008FF (master, HI=15, LO=15, S_GENERAL_IGNORE)
- HSIOM PRT8: 0x00000F0F
- GPIO CFG pins 0,1: 0x0C (OD_DRIVESLOW)

## What's Left To Try (Next Session)

### Priority 1: Power cycle and test with DPLL already configured
Since DPLL is already running, just boot our app without touching DPLL:
1. Power cycle board (USB unplug/replug)
2. Flash RRAM image (no DPLL DTS config) with SW6=OFF
3. Switch SW6=OFF → boot our app
4. Check CLK_ROOT10 value and I2C scan result

### Priority 2: Enable DPLL in DTS properly
The Zephyr clock driver DOES support DPLL but may need specific setup:
1. Add clock-output-names to path-mux nodes
2. Verify dpll_lp0 is sourced from correct path_mux
3. Check if CLK_HF10 needs explicit reconfiguration after DPLL enable

### Priority 3: Fix sysbuild M33 companion HardFault
Our sysbuild companion HardFaults in `ifx_pse84_cm55_startup()`. The fault might be in:
- `cy_sau_init()`
- `Cy_SysEnableCM55()`
- `cy_mpc_init()` or `cy_ppc0_init()`

Debug with OpenOCD GDB attach to find the exact fault location.

## Files
- I2C test: `zephyr_workspace/pse84_i2c_test/`
- Display app: `zephyr_workspace/pse84_display/`
- Infineon BSP: `ifx_bsp/PSOC_Edge_Graphics_LVGL_Smartwatch_Demo/bsps/TARGET_APP_KIT_PSE84_EVAL_EPC2/`
- Waveshare driver: `ifx_bsp/mtb_shared/display-dsi-waveshare-4-3-lcd/`
- Zephyr clock driver: `zephyr/drivers/clock_control/clock_control_infineon_fixed_clock.c`

## Build/Flash (for reference)
```bash
# Standalone M33 (RRAM, SW6=OFF)
west build -b kit_pse84_eval/pse846gps2dbzc4a/m33 -p always ../pse84_i2c_test
edgeprotecttools image-metadata --image build/zephyr/zephyr.hex \
  --output build/zephyr/zephyr.signed_ept.hex \
  --erased-val 0 --hex-addr 0x22011000 --header-size 0x400
/Applications/ModusToolboxProgtools-1.8/openocd/bin/openocd \
  -f zephyr/boards/infineon/kit_pse84_eval/support/openocd.cfg \
  -c "gdb_port disabled; telnet_port disabled; tcl_port disabled" \
  -c "init; targets cat1d.cm33; reset halt; flash write_image erase build/zephyr/zephyr.signed_ept.hex; reset run; shutdown"

# Sysbuild M55 (SMIF, SW6=ON)
west build -b kit_pse84_eval/pse846gps2dbzc4a/m55 --sysbuild -p always ../pse84_i2c_test
# Sign both, flash M33 to RRAM and M55 to SMIF with SMIF_BANKS
```
