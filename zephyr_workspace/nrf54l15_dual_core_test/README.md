# nRF54L15 Dual-Core BLE + RISC-V Test

Dual-core application for nRF54L15 that combines BLE throughput testing on the ARM Cortex-M33 core with workload simulation and MIPS measurement on the RISC-V (FLPR) core.

## Architecture

- **ARM Cortex-M33 (cpuapp)**: BLE stack, throughput test, IPC master
- **RISC-V (cpuflpr)**: Workload simulation, MIPS measurement, IPC slave

The cores communicate via IPC (Inter-Processor Communication) to:
- ARM sends workload commands to RISC-V
- RISC-V sends performance stats back to ARM
- ARM exposes RISC-V control via BLE GATT characteristic

## Features

### ARM Core (Cortex-M33 @ 128 MHz)
1. BLE throughput test (same as single-core version)
2. Configurable TX rate for both Mac and device
3. MIPS estimation for BLE processing
4. IPC communication with RISC-V
5. BLE GATT characteristic to control RISC-V workloads

### RISC-V Core (FLPR @ 64 MHz)
1. Multiple workload simulations:
   - **0**: Idle (no workload)
   - **1**: Matrix multiplication (4x4 matrices)
   - **2**: Sorting (bubble sort, 32 elements)
   - **3**: FFT simulation (butterfly operations)
   - **4**: Crypto simulation (AES-like operations)
   - **5**: Mixed (all workloads combined)
2. Cycle counting and MIPS measurement
3. Real-time stats reporting via IPC

## BLE GATT Service

**Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`

**Characteristics**:
- `6E400003`: TX (notify) - Device sends data to Mac
- `6E400002`: RX (write) - Mac sends data to device
- `6E400004`: Control (write) - Set device TX rate (4-byte uint32, kbps)
- `6E400005`: RISC-V Workload (write) - Set RISC-V workload type (1-byte uint8, 0-5)

## Building

### Prerequisites
- Zephyr workspace set up (see main README)
- nRF54L15 DK
- Python environment with `west` tool

### Build Command

```bash
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_dual_core_test --sysbuild
```

**IMPORTANT**: Must use `--sysbuild` flag to build both cores!

### Clean Build

```bash
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_dual_core_test --sysbuild -p
```

### Flash

```bash
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash
```

This will flash both the ARM and RISC-V images to the device.

## Usage

### 1. Flash and Monitor

```bash
# Flash firmware
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash

# Monitor serial output (115200 baud)
screen /dev/tty.usbmodem* 115200
```

### 2. Connect via BLE

Use the Python test script to connect and control the device:

```bash
cd /Users/danahern/Downloads/zephyr_workspace
source ble_test_env/bin/activate

# Max speed test
python3 ble_throughput_test.py

# Control TX rates
python3 ble_throughput_test.py --mac-tx 100 --device-tx 50
```

### 3. Control RISC-V Workload

Currently, you need to use a BLE tool (like nRF Connect) to write to the RISC-V workload characteristic (`6E400005`):

**Workload Values**:
- `0x00` - Idle
- `0x01` - Matrix multiplication
- `0x02` - Sorting
- `0x03` - FFT simulation
- `0x04` - Crypto simulation
- `0x05` - Mixed workload

The ARM core will relay the command to the RISC-V core via IPC, and RISC-V stats will appear in the serial output.

## Serial Output Example

```
Starting nRF54L15 Dual-Core BLE Test (ARM Cortex-M33)
IPC initialized for RISC-V communication
Bluetooth initialized
Advertising successfully started
Device name: nRF54L15_Dual
Waiting for connection...

Starting RISC-V Core (FLPR)
CPU Frequency: 64 MHz (assumed)
RISC-V: IPC initialized
RISC-V: Ready for workload commands

Connected: XX:XX:XX:XX:XX:XX
ARM: Set RISC-V workload to 1

=== Performance Stats ===
TX: 24400 bytes (195 kbps)
RX: 12200 bytes (97 kbps)
Total: 36600 bytes
CPU freq: 128 MHz
Est. active MIPS (BLE processing): ~85
Est. CPU utilization: ~66%

--- RISC-V Core Stats ---
Workload: 1
Est. MIPS: 12
-------------------------
========================
```

## Workload Descriptions

### 1. Matrix Multiplication
- 4x4 matrix multiplication
- Tests integer arithmetic and memory access
- Moderate CPU load

### 2. Sorting
- Bubble sort on 32-element array
- Tests branching and memory operations
- Variable load depending on data

### 3. FFT Simulation
- Simulated butterfly operations (16-point)
- Tests complex arithmetic patterns
- Moderate to high CPU load

### 4. Crypto Simulation
- AES-like substitution and mixing operations
- Tests bit manipulation and memory access
- High CPU load

### 5. Mixed Workload
- Runs all workloads sequentially
- Most comprehensive test
- Highest CPU load

## Directory Structure

```
nrf54l15_dual_core_test/
├── CMakeLists.txt                      # Main build file
├── prj.conf                            # ARM core configuration
├── sysbuild.cmake                      # Dual-core build configuration
├── nrf54l15dk_nrf54l15_cpuapp.overlay  # Device tree overlay
├── cpuapp/                             # ARM Cortex-M33 application
│   ├── src/
│   │   └── main.c                      # BLE + IPC code
├── cpuflpr/                            # RISC-V application
│   ├── CMakeLists.txt
│   ├── prj.conf
│   └── src/
│       └── main.c                      # Workload simulation + IPC
└── README.md                           # This file
```

## Troubleshooting

### Build fails with "sysbuild not found"
Make sure you're using `--sysbuild` flag in the build command.

### IPC not working
- Check that both cores are flashed
- Verify device tree overlay enables `cpuflpr_vpr` and `ipc0`
- Check serial output for IPC initialization messages

### RISC-V workload not changing
- Verify BLE characteristic write is successful
- Check ARM core received the command (serial output)
- Ensure IPC is initialized on both cores

### No RISC-V stats in output
- RISC-V needs to send stats first (happens every second)
- Try setting a workload (non-idle) to generate activity
- Check IPC communication is working

## Performance Notes

**ARM Core (128 MHz)**:
- BLE at max throughput (~185 kbps): ~85 MIPS, ~66% CPU
- BLE at 50 kbps: ~23 MIPS, ~17% CPU

**RISC-V Core (64 MHz)**:
- Matrix multiplication: ~10-15 MIPS
- Sorting: ~8-12 MIPS (variable)
- FFT simulation: ~15-20 MIPS
- Crypto simulation: ~20-25 MIPS
- Mixed workload: ~50-60 MIPS

## Future Enhancements

1. Add Python script support for RISC-V workload control
2. Add more realistic workloads (audio processing, sensor fusion)
3. Add power measurement integration
4. Add bidirectional workload coordination (ARM tells RISC-V about BLE load)
5. Add memory usage statistics

## References

- [Zephyr IPC Service Documentation](https://docs.zephyrproject.org/latest/services/ipc/index.html)
- [nRF54L15 FLPR Core](https://docs.nordicsemi.com/bundle/ps_nrf54l15/page/flpr.html)
- [Zephyr Sysbuild](https://docs.zephyrproject.org/latest/build/sysbuild/index.html)
