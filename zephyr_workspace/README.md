# Zephyr RTOS Development Environment for nRF54L15

This workspace contains a complete Zephyr RTOS development environment with **two applications** for the nRF54L15 development kit:
1. **Single-core BLE throughput test** (ARM Cortex-M33 only)
2. **Dual-core BLE + RISC-V workload test** (ARM + RISC-V with IPC)

## Directory Structure

```
zephyr_workspace/
├── zephyrproject/              # Zephyr RTOS SDK and modules
│   ├── zephyr/                # Main Zephyr kernel and subsystems
│   ├── modules/               # Hardware abstraction layers
│   ├── bootloader/            # MCUboot bootloader
│   └── build/                 # Build output directory
├── nrf54l15_ble_test/         # Single-core BLE throughput test (ARM only)
│   ├── src/main.c
│   ├── CMakeLists.txt
│   ├── prj.conf
│   └── nrf54l15dk_nrf54l15_cpuapp.overlay
├── nrf54l15_dual_core_test/   # Dual-core BLE + RISC-V test
│   ├── cpuapp/                # ARM Cortex-M33 (BLE + IPC master)
│   │   └── src/main.c
│   ├── cpuflpr/               # RISC-V (workload + IPC slave)
│   │   ├── src/main.c
│   │   ├── CMakeLists.txt
│   │   └── prj.conf
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── sysbuild.cmake
│   ├── nrf54l15dk_nrf54l15_cpuapp.overlay
│   └── README.md
├── ble_test_env/              # Python virtual environment for testing
├── ble_throughput_test.py     # Automated BLE throughput test script
└── README.md                  # This file
```

## Table of Contents

1. [Environment Setup](#environment-setup)
2. [Applications Overview](#applications-overview)
3. [Single-Core BLE Test](#single-core-ble-test)
4. [Dual-Core BLE + RISC-V Test](#dual-core-ble--risc-v-test)
5. [Python Test Script](#python-ble-test-script)
6. [Development History](#development-history)
7. [Troubleshooting](#troubleshooting)
8. [Version Information](#version-information)

---

## Environment Setup

### Python Virtual Environment (Zephyr)

This project uses **pyenv** with a dedicated Python 3.11.11 virtual environment named `zephyr-env` for Zephyr development.

**Location:** `/Users/danahern/.pyenv/versions/zephyr-env/`

**Key tools installed:**
- `west` - Zephyr's meta-tool for managing repositories and building
- All Zephyr Python dependencies (see requirements in `zephyrproject/zephyr/scripts/requirements.txt`)

### Python Virtual Environment (BLE Testing)

A separate venv is used for the BLE test script.

**Location:** `zephyr_workspace/ble_test_env/`

**Key tools installed:**
- `bleak` - Python BLE library for testing

### Zephyr SDK

**Location:** `/Users/danahern/zephyr-sdk-0.17.4/`

**Includes:**
- ARM toolchain (for nRF54L15 Cortex-M33)
- All other architecture toolchains
- CMake packages for building

---

## Initial Setup (Already Completed)

The following steps were performed to set up this environment. **You do not need to repeat these steps** - they are documented for reference and future environments.

### 1. Install pyenv and Python

```bash
# Install pyenv via Homebrew
brew install pyenv pyenv-virtualenv

# Install Python 3.11.11
/opt/homebrew/bin/pyenv install 3.11.11

# Create virtual environment for Zephyr
/opt/homebrew/bin/pyenv virtualenv 3.11.11 zephyr-env
```

### 2. Install West and Dependencies

```bash
# Install west
/Users/danahern/.pyenv/versions/zephyr-env/bin/pip install west

# Initialize Zephyr workspace
cd /Users/danahern/Downloads/zephyr_workspace
/Users/danahern/.pyenv/versions/zephyr-env/bin/west init -m https://github.com/zephyrproject-rtos/zephyr

# Update all repositories (downloads ~2GB)
/Users/danahern/.pyenv/versions/zephyr-env/bin/west update

# Install Python dependencies
/Users/danahern/.pyenv/versions/zephyr-env/bin/pip install -r zephyrproject/zephyr/scripts/requirements.txt
```

### 3. Install Zephyr SDK

```bash
# Install SDK and export environment
/Users/danahern/.pyenv/versions/zephyr-env/bin/west sdk install
/Users/danahern/.pyenv/versions/zephyr-env/bin/west zephyr-export
```

### 4. Set Up BLE Test Environment

```bash
# Create venv for BLE testing
cd /Users/danahern/Downloads/zephyr_workspace
python3 -m venv ble_test_env

# Install dependencies
source ble_test_env/bin/activate
pip install bleak
```

---

## Applications Overview

This workspace contains two complementary applications:

### 1. Single-Core BLE Test (`nrf54l15_ble_test/`)

**Purpose:** Baseline BLE throughput testing on ARM Cortex-M33 core only

**Features:**
- BLE peripheral with custom throughput service
- Configurable device TX rate via BLE characteristic
- Real-time throughput measurement (TX/RX)
- MIPS estimation for BLE processing
- ~185 kbps max throughput per direction

**When to use:**
- Baseline BLE performance testing
- Simpler application without RISC-V complexity
- Focus solely on BLE stack performance

### 2. Dual-Core BLE + RISC-V Test (`nrf54l15_dual_core_test/`)

**Purpose:** Combined BLE throughput testing + RISC-V workload simulation with IPC

**Features:**
- **ARM Core (128 MHz):** Full BLE throughput test + IPC master
- **RISC-V Core (64 MHz):** 5 workload simulations + MIPS measurement + IPC slave
- Inter-processor communication (IPC) between cores
- Control RISC-V workloads via BLE characteristic
- Simultaneous MIPS measurement on both cores
- Workloads: Matrix mult, Sorting, FFT, Crypto, Mixed

**When to use:**
- Testing combined BLE + computational workloads
- Measuring MIPS under realistic multi-core scenarios
- Evaluating IPC performance
- Simulating audio processing, sensor fusion, or similar workloads

---

## Single-Core BLE Test

### Building

### Quick Build

From the workspace root:

```bash
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_ble_test
```

### Clean Build

```bash
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_ble_test -p
```

The `-p` flag performs a pristine build (clean).

### Build Output

**Binary location:** `zephyrproject/build/zephyr/zephyr.elf`

**Build statistics:**
- Flash usage: ~161KB (11% of 1.4MB)
- RAM usage: ~34KB (18% of 188KB)

---

## Flashing the Application

### Using West

```bash
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash
```

### Requirements
- nRF54L15 DK connected via USB
- J-Link drivers installed (usually comes with nRF Connect for Desktop)

---

## Application Details: nRF54L15 BLE Test

### Purpose
Measures BLE streaming performance and estimates MIPS (Million Instructions Per Second) during data transfer between nRF54L15 and a connected device.

### Features

1. **Custom BLE GATT Service**
   - Service UUID: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
   - TX Characteristic (notify): `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
   - RX Characteristic (write): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
   - Compatible with Nordic UART Service (NUS) UUIDs for easy testing

2. **Performance Monitoring**
   - Real-time TX/RX throughput (kbps)
   - Total bytes transferred
   - CPU utilization estimation
   - MIPS estimation based on CPU frequency (128 MHz)
   - Stats printed to UART console every second

3. **Optimizations**
   - BLE 2M PHY for higher throughput
   - 244-byte packet size (near maximum MTU)
   - 7.5ms connection interval (6 * 1.25ms)
   - Data length extension enabled (251 bytes)
   - Delayed connection parameter updates (1 second after connection to stabilize)

4. **Stability Features**
   - Increased BLE stack sizes to prevent overflow
   - Reduced logging verbosity (INFO level instead of DEBUG)
   - Delayed PHY and connection parameter updates
   - Proper buffer management

### Testing the Application

#### Method 1: Manual Testing with nRF Connect Mobile

1. **Flash firmware to nRF54L15 DK**
2. **Connect to serial console** (115200 baud)
   ```bash
   screen /dev/tty.usbmodem* 115200
   ```
3. **Connect via BLE:**
   - Use **nRF Connect** (iOS/Android)
   - Look for device named: `nRF54L15_Test`
   - Connect to the device
   - You should see "Connected" on serial console
   - Wait for PHY update and connection parameter update messages

4. **Enable notifications:**
   - Find TX characteristic (`6E400003...`)
   - Enable notifications
   - Device will start sending data automatically

5. **Send data:**
   - Write to RX characteristic (`6E400002...`)
   - Device receives and counts data
   - Performance stats update every second

6. **Monitor performance:**
   - Watch serial console for stats every second
   - Metrics include: TX/RX kbps, total bytes, CPU utilization, MIPS estimate

#### Method 2: Automated Testing with Python Script

See [Python BLE Test Script](#python-ble-test-script) section below.

### Example Serial Output

```
Starting nRF54L15 BLE Throughput Test
Bluetooth initialized
Advertising successfully started
Device name: nRF54L15_Test
Waiting for connection...
Connected: XX:XX:XX:XX:XX:XX
Requesting PHY update to 2M and connection params...
PHY updated: TX PHY 2, RX PHY 2
Connection params updated: interval 6, latency 0, timeout 400
TX notifications enabled

=== Performance Stats ===
TX: 24400 bytes (195 kbps)
RX: 12200 bytes (97 kbps)
Total: 36600 bytes
CPU freq: 128 MHz
Est. active MIPS (BLE processing): ~85
Est. CPU utilization: ~66%
========================
```

---

## Python BLE Test Script

An automated Python script is provided for sustained throughput testing: `ble_throughput_test.py`

### Features

- Automatic device scanning and connection
- Bidirectional data streaming
- Real-time throughput statistics
- Configurable TX rate or RX-only mode
- Command-line control

### Usage

```bash
# Activate virtual environment
cd /Users/danahern/Downloads/zephyr_workspace
source ble_test_env/bin/activate

# Max speed bidirectional test (~200 kbps each direction)
python3 ble_throughput_test.py

# Target specific TX rate (e.g., 100 kbps)
python3 ble_throughput_test.py 100

# Target 50 kbps
python3 ble_throughput_test.py 50

# RX-only mode (receive from device, don't send)
python3 ble_throughput_test.py 0
```

### Examples

**Max speed test:**
```bash
python3 ble_throughput_test.py
# Sends and receives at maximum speed (~183-185 kbps each direction)
```

**Rate-limited test:**
```bash
python3 ble_throughput_test.py 100
# Limits TX to 100 kbps, RX runs at full speed
```

**RX-only test (measure device TX performance):**
```bash
python3 ble_throughput_test.py 0
# Only receives data from device, doesn't send any data
# Useful for measuring device TX performance without Mac TX load
```

### Output Example

```
Scanning for nRF54L15_Test...
Found device: nRF54L15_Test (XX:XX:XX:XX:XX:XX)
Connecting...
Connected: True
Enabling notifications...
Notifications enabled
Starting continuous data transmission (MAX SPEED)...

=== Throughput Stats (avg over 5.0s) ===
RX (from device): 112,200 bytes (179.5 kbps)
TX (to device):   114,800 bytes (183.7 kbps)
Total:            227,000 bytes
==================================================
```

### Requirements

- Python 3.7+
- bleak library (`pip install bleak`)
- macOS: Bluetooth permissions must be granted to Terminal/iTerm

---

## Development History

### Initial Development

**Goal:** Create a Zephyr RTOS BLE application for nRF54L15 to measure MIPS during sustained data streaming.

**Steps:**
1. Set up isolated pyenv environment with Python 3.11.11
2. Installed Zephyr SDK 0.17.4 and west tool
3. Created custom BLE GATT service (Nordic UART Service not available in mainline Zephyr)
4. Implemented bidirectional data streaming with 244-byte packets
5. Added performance monitoring with cycle counting and MIPS estimation

### Key Issues Resolved

#### Issue 1: BLE Stack Overflow
**Symptom:** Device crashed with "Stack overflow on CPU 0" on `BT CTLR RX pri` thread

**Root Cause:** BLE RX stack too small for high-throughput data streaming with debug logging

**Solution:**
- Increased `CONFIG_BT_RX_STACK_SIZE=4096`
- Increased `CONFIG_BT_HCI_TX_STACK_SIZE=2048`
- Reduced logging from DEBUG to INFO level (`CONFIG_BT_LOG_LEVEL_INF=y`)
- Increased buffer counts: `CONFIG_BT_BUF_EVT_RX_COUNT=20`, `CONFIG_BT_CONN_TX_MAX=10`

**Reference:** `prj.conf` lines 29-35

#### Issue 2: Connection Instability (LMP Response Timeout)
**Symptom:** Device connected but immediately disconnected with reason 31 (LMP Response Timeout). PHY stayed at 1M instead of upgrading to 2M.

**Root Cause:** Requesting PHY update and connection parameter changes immediately after connection, before link was stable.

**Solution:**
- Implemented delayed work queue to schedule updates 1 second after connection
- Used `k_work_init_delayable()` and `k_work_schedule()` with `K_SECONDS(1)` delay
- Allowed connection to stabilize before requesting parameter changes

**Reference:** `main.c` lines 46, 58-83, 133, 288

#### Issue 3: Incorrect MIPS Calculation
**Symptom:** MIPS showed as 0, CPU frequency reported as 1 MHz instead of 128 MHz

**Root Cause:** `sys_clock_hw_cycles_per_sec()` returns system tick frequency (1 MHz), not CPU frequency

**Solution:**
- Hardcoded actual CPU frequency: `const uint32_t cpu_freq = 128000000;`
- Estimated cycles per byte based on empirical measurements (~2844 cycles/byte)
- Calculated MIPS based on throughput and estimated CPU utilization

**Reference:** `main.c` lines 220-239

#### Issue 4: Bluetooth Permissions on macOS
**Symptom:** Python script failed with "Bluetooth device is turned off"

**Root Cause:** iTerm/Terminal lacked Bluetooth permissions on macOS

**Solution:** Granted Bluetooth permissions to terminal app in System Settings

### Feature Additions

1. **Initial Implementation** - Basic BLE GATT service with TX/RX characteristics
2. **Performance Monitoring** - Added throughput stats, cycle counting, MIPS estimation
3. **Python Test Script** - Automated testing with bleak library
4. **Configurable TX Rate** - Added command-line argument to set target TX rate in kbps
5. **RX-Only Mode** - Added option to disable TX (set to 0) for device TX testing

---

## Setting Up Future Environments

### For Claude AI Assistant

**Context for Claude:**
- This workspace uses Zephyr RTOS 4.2.99 (latest development)
- Python environment for Zephyr: `/Users/danahern/.pyenv/versions/zephyr-env/`
- Python environment for testing: `ble_test_env/` (use `source ble_test_env/bin/activate`)
- Zephyr SDK: `/Users/danahern/zephyr-sdk-0.17.4/`
- Always use absolute paths for west: `/Users/danahern/.pyenv/versions/zephyr-env/bin/west`
- Build from `zephyrproject/` directory
- nRF54L15 board identifier: `nrf54l15dk/nrf54l15/cpuapp`

**Common Commands:**

```bash
# Build
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_ble_test

# Flash
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash

# Clean build
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_ble_test -p

# Menuconfig (modify Kconfig options)
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -t menuconfig

# Run Python test script
cd /Users/danahern/Downloads/zephyr_workspace
source ble_test_env/bin/activate
python3 ble_throughput_test.py [target_kbps]
```

### For New Machine Setup

If setting up on a new machine, follow these steps:

1. **Install prerequisites:**
   ```bash
   # macOS
   brew install cmake ninja gperf python3 ccache qemu dtc wget libmagic
   brew install pyenv pyenv-virtualenv
   ```

2. **Set up Python environment for Zephyr:**
   ```bash
   pyenv install 3.11.11
   pyenv virtualenv 3.11.11 zephyr-env
   ```

3. **Clone and set up Zephyr:**
   ```bash
   mkdir ~/zephyr_workspace && cd ~/zephyr_workspace
   ~/.pyenv/versions/zephyr-env/bin/pip install west
   ~/.pyenv/versions/zephyr-env/bin/west init -m https://github.com/zephyrproject-rtos/zephyr
   ~/.pyenv/versions/zephyr-env/bin/west update
   ~/.pyenv/versions/zephyr-env/bin/pip install -r zephyrproject/zephyr/scripts/requirements.txt
   ```

4. **Install Zephyr SDK:**
   ```bash
   ~/.pyenv/versions/zephyr-env/bin/west sdk install
   ~/.pyenv/versions/zephyr-env/bin/west zephyr-export
   ```

5. **Copy your application:**
   ```bash
   cp -r /path/to/nrf54l15_ble_test ~/zephyr_workspace/
   cp /path/to/ble_throughput_test.py ~/zephyr_workspace/
   ```

6. **Set up BLE test environment:**
   ```bash
   cd ~/zephyr_workspace
   python3 -m venv ble_test_env
   source ble_test_env/bin/activate
   pip install bleak
   ```

---

## Project Configuration Files

### prj.conf
Main Kconfig configuration for the application. Key settings:

**BLE Configuration:**
- `CONFIG_BT=y` - Enable Bluetooth
- `CONFIG_BT_PERIPHERAL=y` - Enable peripheral role
- `CONFIG_BT_DEVICE_NAME="nRF54L15_Test"` - Device name

**Throughput Optimization:**
- `CONFIG_BT_L2CAP_TX_MTU=247` - Maximum MTU for throughput
- `CONFIG_BT_BUF_ACL_TX_SIZE=251` - TX buffer size
- `CONFIG_BT_BUF_ACL_RX_SIZE=251` - RX buffer size
- `CONFIG_BT_CTLR_DATA_LENGTH_MAX=251` - Data length extension
- `CONFIG_BT_CTLR_PHY_2M=y` - Enable 2M PHY

**Stack Sizes (Critical for Stability):**
- `CONFIG_BT_RX_STACK_SIZE=4096` - BLE RX thread stack
- `CONFIG_BT_HCI_TX_STACK_SIZE=2048` - BLE HCI TX thread stack
- `CONFIG_MAIN_STACK_SIZE=2048` - Main thread stack
- `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048` - Work queue stack

**Buffer Counts:**
- `CONFIG_BT_BUF_CMD_TX_COUNT=10` - Command TX buffers
- `CONFIG_BT_BUF_EVT_RX_COUNT=20` - Event RX buffers
- `CONFIG_BT_CONN_TX_MAX=10` - Connection TX buffers

**Performance Monitoring:**
- `CONFIG_TIMING_FUNCTIONS=y` - Enable cycle counting
- `CONFIG_THREAD_MONITOR=y` - Thread monitoring
- `CONFIG_STATS=y` - Statistics collection

### CMakeLists.txt
Build system configuration. Minimal setup that includes `src/main.c`.

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf54l15_ble_test)
target_sources(app PRIVATE src/main.c)
```

### Device Tree Overlay
Hardware-specific configuration for nRF54L15 DK. Enables:
- Radio peripheral (BLE)
- UART for console output (115200 baud)

```dts
&radio {
    status = "okay";
};

&uart20 {
    status = "okay";
    current-speed = <115200>;
};
```

---

## Troubleshooting

### Build Issues

**Problem:** `west: command not found`
```bash
# Use absolute path
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build ...
```

**Problem:** `west: unknown command "build"`
```bash
# Must be in zephyrproject directory
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
```

**Problem:** Kconfig warnings
- Check `prj.conf` for undefined symbols
- Run `west build -t menuconfig` to see available options

**Problem:** Stack overflow during runtime
- Increase relevant stack size in `prj.conf`
- For BLE: `CONFIG_BT_RX_STACK_SIZE`, `CONFIG_BT_HCI_TX_STACK_SIZE`
- Reduce logging verbosity to save stack space

### Flash Issues

**Problem:** `nrfjprog: command not found`
- Install nRF Command Line Tools from Nordic Semiconductor
- Available via nRF Connect for Desktop

**Problem:** Device not detected
- Check USB connection
- Verify J-Link drivers installed
- Try `west flash --recover` to erase and flash

### BLE Connection Issues

**Problem:** Device not advertising
- Check serial console for error messages
- Verify Bluetooth is enabled in `prj.conf`
- Check that board DTS has radio enabled

**Problem:** Device disconnects immediately after connection
- Check for stack overflow on serial console
- Verify connection parameter update timing (should be delayed)
- Ensure PHY update is delayed by at least 1 second

**Problem:** PHY stuck at 1M (not upgrading to 2M)
- Delay PHY update request by 1 second after connection
- Check `CONFIG_BT_CTLR_PHY_2M=y` in `prj.conf`
- Verify central device supports 2M PHY

**Problem:** Low throughput
- Ensure 2M PHY negotiated (check serial output: "TX PHY 2, RX PHY 2")
- Verify connection interval is low (~7.5ms = interval 6)
- Check for BLE interference
- Verify MTU is 247 bytes

### Python Script Issues

**Problem:** `ModuleNotFoundError: No module named 'bleak'`
```bash
# Activate virtual environment first
source ble_test_env/bin/activate
pip install bleak
```

**Problem:** `Bluetooth device is turned off`
- Grant Bluetooth permissions to Terminal/iTerm in System Settings
- Verify macOS Bluetooth is enabled
- Try running with `sudo` (not recommended for security)

**Problem:** Cannot find device
- Ensure nRF54L15 is flashed and advertising (check serial console)
- Verify device name matches (`nRF54L15_Test`)
- Check Bluetooth range (move device closer)
- Try scanning with nRF Connect mobile app first

---

## Additional Resources

- [Zephyr Documentation](https://docs.zephyrproject.org/)
- [nRF54L15 Product Specification](https://www.nordicsemi.com/Products/nRF54L15)
- [Zephyr Bluetooth Documentation](https://docs.zephyrproject.org/latest/connectivity/bluetooth/index.html)
- [West Tool Documentation](https://docs.zephyrproject.org/latest/develop/west/index.html)
- [Bleak Documentation](https://bleak.readthedocs.io/)
- [BLE GATT Specifications](https://www.bluetooth.com/specifications/specs/core-specification-5-3/)

---

## Version Information

- **Zephyr Version:** 4.2.99 (development branch, v4.2.0-5531-gb8419e6d296f)
- **Zephyr SDK:** 0.17.4
- **Python (Zephyr):** 3.11.11 via pyenv
- **Python (Testing):** 3.x via venv
- **West:** 1.5.0
- **Bleak:** Latest (installed via pip)
- **Target Board:** nRF54L15 DK (nrf54l15dk/nrf54l15/cpuapp)
- **CPU:** ARM Cortex-M33 @ 128 MHz
- **Flash:** 1.4 MB
- **RAM:** 188 KB
- **Created:** 2025-10-10
- **Last Updated:** 2025-10-10

---

## Performance Benchmarks

**Achieved Throughput (bidirectional, 2M PHY):**
- TX (device to Mac): ~183-185 kbps
- RX (Mac to device): ~183-185 kbps
- Total: ~360-370 kbps

**CPU Utilization:**
- Estimated: ~66% at full throughput
- Estimated MIPS: ~85 MIPS during BLE processing

**BLE Parameters:**
- PHY: 2M
- Connection Interval: 7.5ms (interval = 6)
- Latency: 0
- Timeout: 400 (4 seconds)
- MTU: 247 bytes
- Packet Size: 244 bytes

---

**Note:** This environment is specific to macOS on Apple Silicon (ARM64). For other platforms (Linux, Windows, x86), paths and some tools may differ. Refer to official Zephyr Getting Started guide for platform-specific instructions.

---

## Dual-Core BLE + RISC-V Test

See **APPLICATIONS_GUIDE.md** for complete documentation.

### Quick Start

```bash
# Build (MUST use --sysbuild flag)
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_dual_core_test --sysbuild -p

# Flash (flashes both cores)
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash

# Test
cd /Users/danahern/Downloads/zephyr_workspace
source ble_test_env/bin/activate
python3 ble_throughput_test.py
```

### Features

- **ARM Core:** BLE throughput test + IPC master
- **RISC-V Core:** 5 workload simulations + IPC slave
- **Workloads:** Matrix multiplication, Sorting, FFT, Crypto, Mixed
- **Control:** BLE characteristic `6E400005` to set RISC-V workload (0-5)
- **Stats:** Simultaneous MIPS measurement on both cores

### Important Notes

1. **MUST use `--sysbuild` flag** when building
2. See `nrf54l15_dual_core_test/README.md` for detailed documentation
3. IPC may show warnings if device tree not fully configured (app still works)
4. Python script works with both single-core and dual-core applications

---

## Summary

You now have TWO complete applications:

1. **`nrf54l15_ble_test/`** - Simple single-core BLE throughput testing
2. **`nrf54l15_dual_core_test/`** - Advanced dual-core with RISC-V workload simulation

Both use the same Python test script (`ble_throughput_test.py`) with flexible rate control via `--mac-tx` and `--device-tx` flags.

**See APPLICATIONS_GUIDE.md for:**
- Quick start commands
- Feature comparison table
- BLE GATT service details
- RISC-V workload descriptions
- Expected performance metrics
- Troubleshooting guide
- Audio streaming calculations

