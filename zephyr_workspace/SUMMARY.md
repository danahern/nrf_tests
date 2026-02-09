# nRF54L15 BLE Workspace — Summary of Findings

## Headline Results

| Test Path | Protocol | Throughput | % of Theoretical Max |
|-----------|----------|-----------|---------------------|
| nRF → nRF (SDC, 50ms CI) | L2CAP CoC | **1317 kbps** | 92% of 1426 kbps |
| nRF → nRF (Zephyr LL, 15ms CI) | L2CAP CoC | **1285 kbps** | 90% |
| nRF → macOS (M4 Max, Tahoe 26.2) | L2CAP CoC | **530 kbps** | ~79% of ~670 kbps Apple ceiling |
| nRF → macOS | GATT Notifications | **680 kbps** | N/A (bidirectional) |
| nRF → iOS (iPhone 17 Pro Max) | L2CAP CoC | **446 kbps** | Below macOS |
| nRF → macOS (baseline GATT) | GATT Notifications | **360 kbps** | Pre-optimization |

## Applications

### 1. `nrf54l15_ble_test/` — Single-Core GATT Throughput Test
- **Purpose**: BLE Nordic UART Service (NUS) bidirectional throughput + MIPS measurement
- **Core**: ARM Cortex-M33 only
- **Protocol**: GATT notifications (ATT_MTU up to 527, 2M PHY)
- **Advertises as**: `nRF54L15_Test`
- **Build**: `west build -b nrf54l15dk/nrf54l15/cpuapp` (Zephyr tree)
- **Key result**: 680 kbps after optimization (89% improvement from 360 kbps baseline)

### 2. `nrf54l15_dual_core_test/` — Dual-Core BLE + RISC-V
- **Purpose**: BLE on ARM + workload simulation on RISC-V via IPC
- **Cores**: ARM Cortex-M33 (BLE master) + RISC-V FLPR (workload slave)
- **RISC-V workloads**: Idle, matrix multiply, sorting, FFT, crypto simulation
- **Build**: Sysbuild (`west build` with sysbuild.cmake)
- **Key finding**: RISC-V runs workloads concurrently with BLE on ARM without throughput impact

### 3. `nrf54l15_l2cap_test/` — L2CAP CoC Peripheral (macOS/iOS optimized)
- **Purpose**: Maximum throughput peripheral for Apple centrals
- **Protocol**: L2CAP CoC (bypasses GATT overhead), GATT exposes PSM
- **Controller**: Zephyr LL (optimal for macOS at 15ms CI)
- **Config**: SDU=492, 6 TX buffers, semaphore=6, CI=15ms
- **Advertises as**: `nRF54L15_Test`
- **Build**: `cd zephyr_workspace/zephyrproject && west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_l2cap_test -p`
- **Key result**: 530 kbps to macOS (hard ceiling), 446 kbps to iOS

### 4. `nrf54l15_l2cap_test_fast/` — L2CAP CoC Peripheral (nRF-to-nRF optimized)
- **Purpose**: Maximum throughput peripheral for nRF central
- **Controller**: Nordic SoftDevice Controller (SDC) with CE extension
- **Config**: SDU=2000, 10 TX buffers, batch credits (80 initial, 10-per-10), CI=50ms
- **Build**: `cd /opt/nordic/ncs/v3.2.1 && nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- west build ...` (NCS tree, SDC requires NCS)
- **Key result**: 1317 kbps (92% theoretical max)

### 5. `nrf54l15_l2cap_central_fast/` — L2CAP CoC Central (nRF-to-nRF)
- **Purpose**: nRF54L15 acting as BLE central for L2CAP CoC reception
- **Key feature**: Uses `seg_recv` API + batch credit replenishment (10-per-10)
- **Controller**: Nordic SDC, CI=50ms, 80 initial credits
- **Build**: Same NCS toolchain as `_fast` peripheral
- **Key result**: Pairs with `_test_fast` for 1317 kbps

### 6. `L2CAPTest/` — iOS L2CAP Throughput Test App
- **Purpose**: Native iOS app to test L2CAP CoC throughput from iPhone
- **Language**: Swift / SwiftUI
- **Files**: `Sources/BLEManager.swift`, `ContentView.swift`, `L2CAPTestApp.swift`
- **Build**: Open `.xcodeproj` in Xcode, deploy to iPhone
- **Key result**: 446 kbps on iPhone 17 Pro Max (worse than macOS's 530 kbps)

### 7. Test Scripts (in `zephyr_workspace/`)

| Script | Language | Purpose |
|--------|----------|---------|
| `l2cap_throughput_test.py` | Python (PyObjC) | macOS L2CAP CoC central — connects, opens channel, measures RX throughput |
| `l2cap_throughput_native.swift` | Swift | Native macOS L2CAP test — confirms Python is not the bottleneck (~504 kbps vs ~520 kbps) |
| `ble_throughput_test.py` | Python (bleak) | GATT notification throughput test |
| `serial_monitor.py` | Python | Safe serial port reader — resets device, captures 60s of logs |

## Key Findings

### GATT Optimization (360 → 680 kbps, +89%)
- **MTU**: 247 → 498 bytes (+89% payload per packet)
- **Connection Interval**: macOS rejects < 15ms; best results at 15ms
- **PHY**: 2M throughout (already optimal)
- **Bottleneck**: macOS enforces 30ms CI for GATT, limiting throughput

### L2CAP CoC — nRF-to-nRF (62 → 1317 kbps)
- **seg_recv API**: Using `bt_l2cap_chan_recv()` (1 credit) = 62 kbps. Switching to `seg_recv` + bulk credits = 1285 kbps — a **20x improvement**.
- **SDC vs Zephyr LL**: SDC enables stable operation at CI > 15ms. Zephyr LL crashes at CI < 15ms under sustained L2CAP load.
- **Connection Interval**: Optimal at 50ms for SDC. Longer CIs pack more PDUs per connection event, reducing CE transition overhead.
- **Credit strategy**: 80 initial credits + replenish 10-per-10 segments is optimal. 1-per-segment wastes airtime on credit PDUs. 20-per-20 causes starvation dips.
- **Theoretical limit**: At 2M PHY + DLE=251 + 50ms CI, max is 1426 kbps. Achieved 1317 kbps = 92% efficiency.

### L2CAP CoC — macOS Ceiling (530 kbps)
Exhaustively tested on M4 Max MacBook Pro / macOS Tahoe 26.2:

- **CI must be 15ms**: Shorter (11.25ms, 12.5ms) = macOS rejects, ~280 kbps. Longer (50ms) = same PDUs/CE but fewer CEs = 165 kbps.
- **~5 PDUs per connection event**: macOS caps this regardless of CI length, controller type, or buffer count.
- **SDU=492 optimal**: 1-fragment (247) = too much header overhead. 3-fragment (741) = credit/reassembly overhead.
- **SDC CE extension doesn't help**: macOS decides when to stop, not the peripheral.
- **Python vs native Swift**: Identical results (504 vs 520 kbps). Python/PyObjC is NOT the bottleneck.
- **8 firmware experiments + 2 central implementations** all confirm 530 kbps ceiling.

### iOS Results (446 kbps)
- Worse than macOS (446 vs 530 kbps)
- Investigation needed: DLE negotiation timing, CI enforcement differences
- iOS backgrounding restrictions may affect throughput

## Hardware

| Device | Role | Serial Number |
|--------|------|--------------|
| nRF54L15 DK #1 | Peripheral | 1057709871 |
| nRF54L15 DK #2 | Central | 1057725401 |
| M4 Max MacBook Pro 16" (Nov 2024) | macOS central | — |
| iPhone 17 Pro Max | iOS central | — |

## Build Environments

| Environment | Use Case | Command Prefix |
|-------------|----------|---------------|
| Zephyr (open-source LL) | macOS-optimized peripheral | `cd zephyr_workspace/zephyrproject && west build ...` |
| NCS v3.2.1 (SDC) | nRF-to-nRF optimized builds | `cd /opt/nordic/ncs/v3.2.1 && nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- west build ...` |
| Xcode | iOS app | Open `L2CAPTest.xcodeproj` |
| swiftc -O | Native macOS test | `swiftc -O -o l2cap_test_native l2cap_throughput_native.swift` |

## Detailed Documentation

| Document | Contents |
|----------|----------|
| `OPTIMIZATION_SUMMARY.md` | GATT notification optimization journey (360 → 680 kbps) |
| `BLE_OPTIMIZATION_GUIDE.md` | Detailed GATT tuning guide (MTU, CI, PHY, buffers) |
| `APPLICATIONS_GUIDE.md` | Comparison of single-core vs dual-core apps, RISC-V workloads |
| `nrf54l15_l2cap_test/PLAN.md` | Full L2CAP optimization history (20+ experiments) |
| `nrf54l15_l2cap_test/MACOS_OPTIMIZATION.md` | macOS throughput ceiling analysis (9 experiments + native Swift) |
| `nrf54l15_l2cap_test_fast/THROUGHPUT_LOG.md` | nRF-to-nRF SDC optimization (11 experiments, 1317 kbps) |
