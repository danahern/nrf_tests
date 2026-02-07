# nRF54L15 Applications Guide

## Quick Start

### Single-Core BLE Test
```bash
# Build
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_ble_test -p

# Flash
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash

# Test with Python
cd /Users/danahern/Downloads/zephyr_workspace
source ble_test_env/bin/activate
python3 ble_throughput_test.py --mac-tx 100 --device-tx 50
```

### Dual-Core BLE + RISC-V Test
```bash
# Build (note: --sysbuild flag is REQUIRED)
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject  
/Users/danahern/.pyenv/versions/zephyr-env/bin/west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_dual_core_test --sysbuild -p

# Flash (flashes both ARM and RISC-V cores)
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash

# Test with Python (same script works for both applications)
cd /Users/danahern/Downloads/zephyr_workspace
source ble_test_env/bin/activate
python3 ble_throughput_test.py
```

## Detailed Comparison

| Feature | Single-Core | Dual-Core |
|---------|-------------|-----------|
| **Cores Used** | ARM only | ARM + RISC-V |
| **BLE Support** | Yes | Yes |
| **RISC-V Workloads** | No | Yes (5 types) |
| **IPC** | No | Yes |
| **Build Command** | `west build ...` | `west build ... --sysbuild` |
| **Complexity** | Simple | Advanced |
| **Use Case** | BLE testing | Multi-core + workload testing |

## BLE GATT Service (Both Applications)

**Service UUID:** `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`

**Characteristics:**

| UUID | Type | Purpose | Single-Core | Dual-Core |
|------|------|---------|-------------|-----------|
| `6E400003` | TX (notify) | Device sends data to Mac | ✓ | ✓ |
| `6E400002` | RX (write) | Mac sends data to device | ✓ | ✓ |
| `6E400004` | Control (write) | Set device TX rate (uint32 kbps) | ✓ | ✓ |
| `6E400005` | RISC-V Workload (write) | Set RISC-V workload (uint8 0-5) | ✗ | ✓ |

## RISC-V Workloads (Dual-Core Only)

Write to characteristic `6E400005` to control:

| Value | Workload | Description | Est. MIPS @ 64MHz |
|-------|----------|-------------|-------------------|
| `0x00` | Idle | No workload | 0 |
| `0x01` | Matrix Mult | 4x4 matrix multiplication | 10-15 |
| `0x02` | Sorting | Bubble sort (32 elements) | 8-12 |
| `0x03` | FFT Simulation | Butterfly operations (16-point) | 15-20 |
| `0x04` | Crypto Simulation | AES-like operations | 20-25 |
| `0x05` | Mixed | All workloads combined | 50-60 |
| `0x06` | **Audio Pipeline** | **Full audio processing: 3 mics @ 8kHz, beamforming, VAD, IPC** | **80-120** |

## Python Test Script

Works with both applications!

### Basic Usage (Single-Core or Dual-Core)

```bash
# Max speed bidirectional
python3 ble_throughput_test.py

# Control Mac TX rate
python3 ble_throughput_test.py --mac-tx 100

# Control device TX rate
python3 ble_throughput_test.py --device-tx 50

# Control both
python3 ble_throughput_test.py --mac-tx 100 --device-tx 50

# Mac RX only (don't send to device)
python3 ble_throughput_test.py --mac-tx 0
```

### RISC-V Workload Control (Dual-Core Only)

```bash
# Idle (no workload)
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 0

# Matrix multiplication
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 1

# Sorting
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 2

# FFT simulation
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 3

# Crypto simulation
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 4

# Mixed workload
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 5

# Audio pipeline (3 mics, beamforming, VAD)
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 6

# Combine with BLE throughput control
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 6 --device-tx 48

# Show help
python3 ble_throughput_test.py --help
```

## Expected Performance

### Single-Core BLE Test
- **Max BLE Throughput:** ~185 kbps per direction
- **ARM MIPS (BLE at max):** ~85 MIPS
- **ARM CPU Utilization:** ~66%

### Dual-Core BLE + RISC-V Test  
- **Max BLE Throughput:** ~185 kbps per direction (same as single-core)
- **ARM MIPS (BLE at max):** ~85 MIPS
- **RISC-V MIPS (workload dependent):**
  - Idle: 0 MIPS
  - Matrix Mult: ~10-15 MIPS
  - Sorting: ~8-12 MIPS
  - FFT: ~15-20 MIPS
  - Crypto: ~20-25 MIPS
  - Mixed: ~50-60 MIPS

## Troubleshooting

### Build Issues

**Problem:** Dual-core build fails with "sysbuild" error
```bash
# Solution: Add --sysbuild flag
west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_dual_core_test --sysbuild -p
```

**Problem:** Missing Python modules (jsonschema, pyyaml, etc.)
```bash
# Solution: Install in system Python (needed for CMake)
python3 -m pip install --break-system-packages jsonschema pyyaml pykwalify
```

### Runtime Issues

**Problem:** IPC warnings in dual-core app
- This is normal if device tree doesn't have IPC configured
- Application will still work, RISC-V just won't communicate with ARM
- To fix: Proper IPC device tree configuration needed

**Problem:** RISC-V workload not changing
- Ensure you're writing to correct characteristic (`6E400005`)
- Value should be single byte (0-5)
- Check serial output for confirmation messages

## Serial Output Examples

### Single-Core
```
Starting nRF54L15 BLE Throughput Test
Bluetooth initialized
Advertising successfully started
Device name: nRF54L15_Test
Waiting for connection...
Connected: XX:XX:XX:XX:XX:XX

=== Performance Stats ===
TX: 24400 bytes (195 kbps)
RX: 12200 bytes (97 kbps)
Total: 36600 bytes
CPU freq: 128 MHz
Est. active MIPS (BLE processing): ~85
Est. CPU utilization: ~66%
========================
```

### Dual-Core
```
Starting nRF54L15 Dual-Core BLE Test (ARM Cortex-M33)
WARNING: IPC not configured in device tree
Bluetooth initialized
Device name: nRF54L15_Dual

Starting RISC-V Core (FLPR)
CPU Frequency: 64 MHz (assumed)
RISC-V: Ready for workload commands

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

## Audio Pipeline Workload (Workload 6)

The audio pipeline workload simulates a complete real-time audio processing system on the RISC-V core:

### Pipeline Stages

1. **Microphone Input Simulation** (3 mics @ 8kHz)
   - 128 samples per frame (16ms)
   - 12-bit ADC simulation
   - Independent data streams per mic

2. **Pre-Processing**
   - DC offset removal
   - 3-tap FIR filtering
   - Noise floor estimation

3. **Beamforming**
   - Delay-and-sum algorithm
   - Weighted mic contributions (center: 0.5, left/right: 0.25 each)
   - Spatial filtering for target direction

4. **Post-Processing**
   - Noise suppression (spectral subtraction)
   - Automatic Gain Control (AGC)
   - RMS-based level normalization

5. **Voice Activity Detection (VAD)**
   - Energy-based detection
   - Zero-crossing rate analysis
   - Smart thresholding (10-80 crossings, >1000 energy)

6. **IPC Transfer to ARM**
   - Sends processed audio samples via IPC
   - Includes VAD metrics (energy, zero-crossings)
   - Only transmits when voice is detected (bandwidth optimization)

### Expected Performance

- **RISC-V MIPS**: ~80-120 MIPS @ 64 MHz
- **Frame rate**: 62.5 Hz (16ms per frame)
- **Processing latency**: <16ms per frame
- **Voice detection rate**: ~30-50% (depends on simulated data)
- **IPC bandwidth**: Variable (only active frames)

### Testing the Audio Pipeline

```bash
# Flash the dual-core app
cd /Users/danahern/Downloads/zephyr_workspace/zephyrproject
/Users/danahern/.pyenv/versions/zephyr-env/bin/west flash

# Connect via Python and start audio pipeline workload
cd /Users/danahern/Downloads/zephyr_workspace
source ble_test_env/bin/activate
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 6

# Or test with specific BLE throughput
python3 ble_throughput_test.py --name nRF54L15_Dual --workload 6 --device-tx 48
```

The `--workload 6` flag automatically configures the audio pipeline on the RISC-V core.

The serial output will show:
```
--- RISC-V Core Stats ---
Workload: 6
Est. MIPS: 95

--- Audio Pipeline ---
Frames received: 1234
Voice detected: 567
Voice activity: 45%
Frame rate: ~8 kHz sampling
Mics: 3 (beamformed)
Processing: DC removal, FIR filter, beamforming, AGC, VAD
IPC transfer: Active
----------------------
```

### Real-World Application

This workload demonstrates the RISC-V core handling:
- Real-time audio capture from 3 microphones
- Signal processing (filtering, beamforming)
- Computational audio algorithms (AGC, VAD)
- Inter-processor communication with ARM core

The ARM core would then:
- Receive processed audio via IPC
- Compress for BLE transmission
- Run keyword spotting / wake word detection
- Stream to phone/cloud for ASR (Automatic Speech Recognition)

## Next Steps

### For Audio Streaming (3 mics @ 8kHz)

Based on the audio pipeline workload, you'd need ~48-96 kbps for BLE transmission:
- 3 mics × 8 kHz × 8-bit = 192 kbps (too high for BLE)
- With ADPCM compression (4:1): ~48 kbps ✓
- Or reduce to 6 kHz uncompressed: ~144 kbps ✓

**Test with:**
```bash
# Simulate compressed audio data rate
python3 ble_throughput_test.py --name nRF54L15_Dual --device-tx 48  # Compressed audio
python3 ble_throughput_test.py --name nRF54L15_Dual --device-tx 96  # Higher quality
```

### For More Complex Scenarios

1. **Add custom workloads** to RISC-V core (edit `cpuflpr/src/main.c`)
2. **Implement actual IPC** for coordinated tasks
3. **Add power measurement** to correlate with MIPS
4. **Test different PHY modes** (1M vs 2M)
5. **Optimize for lower power** at target throughput

