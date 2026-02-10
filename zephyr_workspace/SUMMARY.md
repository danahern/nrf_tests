# nRF54L15 BLE Workspace — Summary of Findings

## Headline Results

| Test Path | Protocol | Throughput | % of Theoretical Max |
|-----------|----------|-----------|---------------------|
| nRF → nRF (SDC, 50ms CI) | GATT Notifications | **1346 kbps** | 96% of 1400 kbps |
| nRF → nRF (SDC, 50ms CI) | L2CAP CoC | **1317 kbps** | 94% |
| nRF → nRF (Zephyr LL, 15ms CI) | L2CAP CoC | **1285 kbps** | 92% |
| nRF → macOS (M4 Max, Tahoe 26.2) | L2CAP CoC | **530 kbps** | macOS-limited (~5 PDU/CE cap) |
| nRF → macOS (M4 Max, Tahoe 26.2) | GATT Notifications | **500 kbps** | Unidirectional; 670 kbps bidirectional |
| nRF → iOS (iPhone 17 Pro Max) | L2CAP CoC | **446 kbps** | Below macOS |

## Applications

### 1. `nrf54l15_ble_test/` — Single-Core GATT Throughput Test
- **Purpose**: BLE Nordic UART Service (NUS) bidirectional throughput + MIPS measurement
- **Core**: ARM Cortex-M33 only
- **Protocol**: GATT notifications (ATT_MTU up to 527, 2M PHY)
- **Advertises as**: `nRF54L15_Test`
- **Build**: `west build -b nrf54l15dk/nrf54l15/cpuapp` (Zephyr tree)
- **Key result**: ~500 kbps unidirectional to macOS, ~670 kbps bidirectional combined

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

### 6. `nrf54l15_gatt_peripheral_fast/` — GATT Notification Peripheral (nRF-to-nRF optimized)
- **Purpose**: Maximum throughput GATT notification peripheral for nRF central
- **Protocol**: GATT notifications via `bt_gatt_notify_cb()` with semaphore flow control
- **Controller**: Nordic SDC with CE extension
- **Config**: MTU=498 (495 byte payload), 10 TX buffers, semaphore=10, CI=50ms
- **Advertises as**: `nRF54L15_Test` (NUS-compatible UUIDs)
- **Build**: Same NCS toolchain as L2CAP `_fast` peripheral
- **Key result**: 1346 kbps (96% theoretical max) — beats L2CAP CoC by 2%

### 7. `nrf54l15_gatt_central_fast/` — GATT Notification Central (nRF-to-nRF)
- **Purpose**: nRF54L15 acting as BLE central for GATT notification reception
- **Key feature**: Discovers NUS TX characteristic, subscribes to notifications
- **Controller**: Nordic SDC, CI=50ms
- **Build**: Same NCS toolchain as L2CAP `_fast` central
- **Key result**: Pairs with GATT peripheral for 1346 kbps

### 8. `L2CAPTest/` — iOS L2CAP Throughput Test App
- **Purpose**: Native iOS app to test L2CAP CoC throughput from iPhone
- **Language**: Swift / SwiftUI
- **Files**: `Sources/BLEManager.swift`, `ContentView.swift`, `L2CAPTestApp.swift`
- **Build**: Open `.xcodeproj` in Xcode, deploy to iPhone
- **Key result**: 446 kbps on iPhone 17 Pro Max (worse than macOS's 530 kbps)

### 9. Test Scripts (in `zephyr_workspace/`)

| Script | Language | Purpose |
|--------|----------|---------|
| `l2cap_throughput_test.py` | Python (PyObjC) | macOS L2CAP CoC central — connects, opens channel, measures RX throughput |
| `l2cap_throughput_native.swift` | Swift | Native macOS L2CAP test — confirms Python is not the bottleneck (~504 kbps vs ~520 kbps) |
| `ble_throughput_test.py` | Python (bleak) | GATT notification throughput test |
| `serial_monitor.py` | Python | Safe serial port reader — resets device, captures 60s of logs |

## Key Findings

### GATT to macOS (~500 kbps unidirectional)
- **MTU**: 247 → 498 bytes (+89% payload per packet)
- **Connection Interval**: macOS negotiates 15ms; CI > 15ms drops to ~130 kbps
- **PHY**: 2M throughout (already optimal)
- **Bidirectional**: ~670 kbps combined (308 RX + 365 TX). Original "680 kbps" was this combined figure.
- **L2CAP CoC is faster**: 530 kbps vs 500 kbps for unidirectional streaming to macOS
- **SDC hurts macOS**: SDC at 15ms CI = 443 kbps. Zephyr LL at 15ms = 500 kbps. SDC has more per-CE overhead.

### L2CAP CoC — nRF-to-nRF (62 → 1317 kbps)
- **seg_recv API**: Using `bt_l2cap_chan_recv()` (1 credit) = 62 kbps. Switching to `seg_recv` + bulk credits = 1285 kbps — a **20x improvement**.
- **SDC vs Zephyr LL**: SDC enables stable operation at CI > 15ms. Zephyr LL crashes at CI < 15ms under sustained L2CAP load.
- **Connection Interval**: Optimal at 50ms for SDC. Longer CIs pack more PDUs per connection event, reducing CE transition overhead.
- **Credit strategy**: 80 initial credits + replenish 10-per-10 segments is optimal. 1-per-segment wastes airtime on credit PDUs. 20-per-20 causes starvation dips.
- **Theoretical limit**: At 2M PHY + DLE=251 + 50ms CI, max is 1426 kbps. Achieved 1317 kbps = 92% efficiency.

### GATT Notifications — nRF-to-nRF (1346 kbps, 96% theoretical max)
- **Semaphore flow control**: Using `bt_gatt_notify_cb()` with sent callback + semaphore (10 in-flight) instead of sleep-based pacing. Same pattern as L2CAP throughput test.
- **GATT beats L2CAP**: 1346 vs 1317 kbps at 50ms CI (+2.2%). GATT avoids L2CAP credit PDU overhead.
- **50ms CI is optimal for GATT nRF-to-nRF**: At 100ms CI, GATT drops to ~760 kbps (highly variable). Unlike L2CAP, GATT has no credit-based flow control, so longer CIs cause notification pipeline stalls.
- **MTU=498**: 495 byte notification payload (MTU - 3 ATT header).
- **macOS**: GATT gets ~500 kbps unidirectional (Zephyr LL, 15ms CI). L2CAP CoC still wins for macOS.

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
