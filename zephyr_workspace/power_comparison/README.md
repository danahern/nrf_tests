# Power Comparison: nRF54LM20 vs Alif B1

Measures and compares power consumption across four operating modes using a Nordic PPK2 power profiler in source meter mode.

## Test Modes

| Mode | Description | Duration |
|------|-------------|----------|
| **Idle** | Deepest sleep with 1s periodic wakeup | 60s |
| **Advertising** | Non-connectable BLE advertising at 1s interval | 60s |
| **Throughput** | GATT notification streaming (244B payloads) | 120s |
| **L2CAP** | L2CAP CoC streaming (492B SDUs) | 120s |

## Hardware Setup

### nRF54LM20 DK (PCA10184)

1. **Cut solder bridge SB10** to isolate VDD_nRF current path
2. Connect PPK2 **VOUT** to the nRF side of **P14** header
3. Connect PPK2 **GND** to DK ground (TP16-TP20)
4. Keep debugger USB connected (separate power domain)
5. **Disconnect** nRF USB (J5)
6. PPK2 voltage: **1800 mV**

### Alif B1 DevKit (DK-B1)

1. **Cut shunting trace on JP4** (VDD_MAIN) and solder header posts
2. Connect PPK2 **VOUT** to SoC side of JP4
3. Connect PPK2 **GND** to DK ground (TP6-TP11)
4. Keep debug USB (J3) connected for SE-UART flashing (separate LDO)
5. **Disconnect** USB Host (J1)
6. PPK2 voltage: **3300 mV**

> **Note:** Different operating voltages (1.8V vs 3.3V) mean current comparisons are misleading. Compare **power (mW)** and **energy/bit (nJ/bit)** instead.

## Prerequisites

- Nordic PPK2 connected via USB
- `ppk2_api` Python package installed in `zephyr-env`
- `nrfjprog` for nRF flashing
- Alif Security Toolkit for Alif flashing

## Building Firmware

### nRF54LM20 (from `zephyr_workspace/zephyrproject/`)

```bash
# Idle
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_idle_test -d ../nrf54lm20_idle_test/build -p

# Advertising
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_adv_test -d ../nrf54lm20_adv_test/build -p

# Throughput (GATT)
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_throughput_test -d ../nrf54lm20_throughput_test/build -p

# Throughput (L2CAP CoC)
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_l2cap_test -d ../nrf54lm20_l2cap_test/build -p
```

### Alif B1 (from `sdk-alif/` workspace)

The Alif SDK is at `sdk-alif/` (already installed). Must use `gnuarmemb` toolchain (not Zephyr SDK, which has picolibc incompatibility).

```bash
cd sdk-alif

# Idle
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_idle_test \
  -d ../zephyr_workspace/alif_b1_idle_test/build -p

# Advertising
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_adv_test \
  -d ../zephyr_workspace/alif_b1_adv_test/build -p

# Throughput (GATT)
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_throughput_test \
  -d ../zephyr_workspace/alif_b1_throughput_test/build -p

# Throughput (L2CAP CoC)
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_l2cap_test \
  -d ../zephyr_workspace/alif_b1_l2cap_test/build -p
```

**Note:** The Alif BLE firmware uses the Alif custom ROM-based BLE stack (not standard Zephyr BLE). The BLE advertising and throughput tests use the `alif_ble.h` / `gapm` API from `hal_alif`.

## Running Measurements

The PPK2 gates power to the device, so flashing happens through the PPK2 supply. The script handles power-on, flash, and measurement automatically.

### Single Platform

```bash
cd zephyr_workspace/power_comparison

# nRF54LM20 - all modes
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 power_compare_test.py \
    --platform nrf54lm20 \
    --ppk2-port /dev/tty.usbmodemXXXX \
    --modes idle advertising throughput \
    --runs 3

# Alif B1 - all modes
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 power_compare_test.py \
    --platform alif_b1 \
    --ppk2-port /dev/tty.usbmodemXXXX \
    --modes idle advertising throughput \
    --runs 3
```

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `--platform` | `nrf54lm20` or `alif_b1` | required |
| `--ppk2-port` | PPK2 serial port | required |
| `--modes` | Test modes to run | all three |
| `--runs` | Measurement runs per mode | 3 |
| `--output` | Output JSON path | `data/<platform>_power.json` |
| `--serial-number` | J-Link serial (nRF only) | auto-detect |
| `--no-flash` | Skip flashing, use current FW | off |

### Comparison Report

```bash
# Compare two platforms
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 power_compare_analysis.py \
    data/nrf54lm20_power.json data/alif_b1_power.json

# Single platform analysis
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 power_compare_analysis.py \
    data/nrf54lm20_power.json
```

Generates console output and a markdown report at `data/COMPARISON_REPORT.md`.

## Output Format

Results are saved as JSON with per-second current samples:

```json
{
  "config": {
    "platform": "nRF54LM20",
    "ppk2_voltage_mV": 1800,
    "measurement_point": "P14 (VDD nRF, SB10 cut)"
  },
  "measurements": [
    {
      "mode": "idle",
      "run_number": 1,
      "summary": {"avg_uA": 1.2, "avg_mA": 0.001, "avg_mW": 0.002, "peak_uA": 15.3},
      "power_per_second": [{"elapsed_s": 0, "avg_uA": 1.2, "peak_uA": 15.3, ...}]
    }
  ]
}
```

## File Structure

```
power_comparison/
  power_compare_test.py      # Main measurement orchestrator
  power_compare_analysis.py  # Comparison report generator
  ppk2_helper.py             # PPK2 init, measure, power cycle
  flash_helper.py            # nRF (nrfjprog) and Alif (app-write-mram) flash
  platforms.py               # Platform configs and test mode definitions
  README.md                  # This file
  data/                      # Output JSON and reports

nrf54lm20_idle_test/         # Zephyr: deep sleep + RTC wakeup
nrf54lm20_adv_test/          # Zephyr: BLE advertising only
nrf54lm20_throughput_test/   # Zephyr: GATT notification streaming
nrf54lm20_l2cap_test/        # Zephyr: L2CAP CoC streaming

alif_b1_idle_test/           # Zephyr: STOP mode + RTC wakeup
alif_b1_adv_test/            # Zephyr (Alif BLE ROM): BLE advertising only
alif_b1_throughput_test/     # Zephyr (Alif BLE ROM): GATT notification streaming
alif_b1_l2cap_test/          # Zephyr (Alif BLE ROM): L2CAP CoC streaming
```
