# Embedded BLE Testing & Power Comparison

Comparative BLE testing across Nordic nRF54 and Alif Balletto B1 platforms. Measures throughput, power consumption, and energy efficiency using Nordic PPK2.

## Platforms

| Platform | SoC | CPU | BLE | Status |
|---|---|---|---|---|
| **nRF54LM20 DK** (PCA10184) | nRF54LM20 ENGA | Cortex-M33 | BT 5.4, Zephyr BLE stack | Verified on hardware |
| **nRF54L15 DK** (PCA10156) | nRF54L15 | Cortex-M33 | BT 5.4, Zephyr LL / SDC | Verified, 1317 kbps nRF-to-nRF |
| **Alif B1 DK** (DK-B1) | AB1C1F4M51820 | Cortex-M55 | BLE 5.3, ROM-based stack | Firmware builds, SE recovery in progress |

## Project Structure

```
embedded/
├── README.md                          # This file
├── CLAUDE.md                          # Project instructions for Claude Code
├── docs/                              # Hardware documentation (PDFs, schematics)
│   ├── Alif_B1_Data-BRIEF_v1.5.pdf
│   ├── DK-E1C-DK-B1-Quick-Start-v14.pdf
│   ├── 220-00315-D_B1-DevKit_1_030526.pdf
│   ├── AUGD0005-Alif-Security-Toolkit-User-Guide-v1.109.0-1.pdf
│   └── nRF54LM20-DK - Hardware files 0_7_0/
│
├── alif_security_tools/               # Alif SETOOLS v1.109.00
│   ├── app-release-exec-macos/        # macOS executables
│   └── app-release-exec-linux/        # Linux x86-64 executables (for B1 A5 silicon)
│
├── sdk-alif/                          # Alif Zephyr SDK (west workspace, v2.1.0)
│   ├── alif/                          # Alif samples, docs, BLE profiles
│   ├── zephyr/                        # Alif fork of Zephyr (v4.1.0)
│   └── modules/                       # hal_alif, mbedtls, lvgl, etc.
│
├── zephyr_workspace/                  # Main development workspace
│   ├── zephyrproject/                 # Upstream Zephyr (v4.2.x) + west modules
│   │
│   ├── # --- Power Comparison Firmware (nRF54LM20) ---
│   ├── nrf54lm20_idle_test/           # Deep sleep + 1s RTC wakeup
│   ├── nrf54lm20_adv_test/            # Non-connectable BLE advertising
│   ├── nrf54lm20_throughput_test/     # GATT notification streaming (460 kbps)
│   ├── nrf54lm20_l2cap_test/          # L2CAP CoC streaming (434 kbps)
│   │
│   ├── # --- Power Comparison Firmware (Alif B1) ---
│   ├── alif_b1_idle_test/             # STOP mode (Alif PM)
│   ├── alif_b1_adv_test/             # BLE advertising (Alif ROM stack)
│   ├── alif_b1_throughput_test/       # GATT notifications (Alif ROM stack)
│   ├── alif_b1_l2cap_test/            # L2CAP CoC (Alif ROM stack)
│   │
│   ├── # --- Power Comparison Scripts ---
│   ├── power_comparison/              # PPK2 measurement, analysis, QEMU tests
│   │   ├── power_compare_test.py      # Main orchestrator (auto PPK2 + BLE central)
│   │   ├── power_compare_analysis.py  # Side-by-side comparison report
│   │   ├── ble_central.py             # BLE receiver (GATT + L2CAP modes)
│   │   ├── ppk2_helper.py             # PPK2 init, measure, auto-detect
│   │   ├── flash_helper.py            # nRF/Alif flash dispatch
│   │   ├── platforms.py               # Platform + test mode definitions
│   │   ├── run_all.py                 # One-command full comparison
│   │   ├── run_qemu_test.sh           # Build + QEMU validation
│   │   └── sim_test/                  # QEMU validation firmware (M33/M55)
│   │
│   ├── # --- Earlier nRF54L15 Work ---
│   ├── nrf54l15_l2cap_test/           # L2CAP CoC peripheral (Zephyr LL)
│   ├── nrf54l15_l2cap_test_fast/      # L2CAP CoC peripheral (SDC, 1317 kbps)
│   ├── nrf54l15_l2cap_central_fast/   # L2CAP CoC central (SDC, nRF-to-nRF)
│   ├── nrf54l15_ble_test/             # GATT notification peripheral
│   │
│   ├── # --- macOS/iOS Test Tools ---
│   ├── l2cap_throughput_test.py       # L2CAP CoC receiver (CoreBluetooth)
│   ├── power_throughput_test.py       # PPK2 + GATT throughput (single run)
│   ├── power_throughput_batch.py      # PPK2 + GATT throughput (batch)
│   ├── power_analysis.py             # Batch result analysis
│   ├── serial_monitor.py             # Safe serial port reader
│   └── L2CAPTest/                     # iOS Swift L2CAP test app
│
└── .claude/                           # Claude Code config + memory
```

## Throughput Results

| Configuration | Throughput | Energy |
|---|---|---|
| nRF54L15 → nRF54L15 (SDC, L2CAP, 50ms CI) | **1317 kbps** | — |
| nRF54L15 → nRF54L15 (Zephyr LL, L2CAP) | 1285 kbps | — |
| nRF54LM20 → macOS (GATT notifications) | **460 kbps** | — |
| nRF54LM20 → macOS (L2CAP CoC) | **434 kbps** | — |
| nRF54L15 → macOS (GATT, 15ms CI) | ~530 kbps | 71.3 nJ/bit @ 1.8V |
| nRF54L15 → iOS (GATT) | ~446 kbps | — |

## Power Measurement

Previous results (nRF54L15 with PPK2):

| Voltage | Current | Power | Efficiency |
|---|---|---|---|
| 1.8V | 20.39 mA | 36.7 mW | 71.3 nJ/bit |
| 3.0V | 20.44 mA | 61.3 mW | 113.2 nJ/bit |
| 4.0V | 20.43 mA | 81.7 mW | 160.7 nJ/bit |

nRF54LM20 vs Alif B1 comparison pending PPK2 hardware setup.

## Build Requirements

### nRF54LM20

```bash
# From zephyr_workspace/zephyrproject/
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_throughput_test -d ../nrf54lm20_throughput_test/build -p
```

### Alif B1

Requires separate Alif Zephyr SDK workspace and `gnuarmemb` toolchain:

```bash
# From sdk-alif/
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he ../zephyr_workspace/alif_b1_throughput_test \
  -d ../zephyr_workspace/alif_b1_throughput_test/build -p
```

Note: Alif BLE uses a custom ROM-based stack (`alif_ble.h`, `gapm`, `gatt_srv`), not standard Zephyr BLE APIs.

### QEMU Validation

```bash
cd zephyr_workspace/power_comparison
./run_qemu_test.sh
```

Builds all firmware and runs validation on QEMU Cortex-M33 (mps2-an521) and Cortex-M55 (mps3-an547).

## Hardware Assignment

| Role | Device | Serial # |
|---|---|---|
| nRF54LM20 DK | PCA10184 | 1051849098 |
| nRF54L15 Peripheral (TX) | PCA10156 | 1057709871 |
| nRF54L15 Central (RX) | PCA10156 | 1057725401 |
| Alif B1 DK | DK-B1 Rev C (A5) | 1219169773 |

## Known Issues

- **Alif B1 SE unresponsive**: The Secure Enclave on the B1 DK doesn't respond to ISP commands via SE-UART at any baud rate, and J-Link SWD can't connect to any core. Recovery investigation in progress.
- **Alif SETOOLS macOS**: macOS tools can generate TOC for B1 but may not produce correctly signed images. Linux SETOOLS v1.109 required (run via OrbStack/Docker with `--platform linux/amd64`).
- **nRF54LM20 ENGA silicon**: Engineering sample — power characteristics may differ from production.
