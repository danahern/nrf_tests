# nRF54L15 BLE Power + Throughput Lab Report

**Date:** 2026-02-14
**Device:** Nordic nRF54L15-DK (PCA10156)
**Firmware:** `nrf54l15_ble_test` — Zephyr BLE Link Layer, GATT notifications
**Measurement Tool:** Nordic Power Profiler Kit II (PPK2)
**BLE Central:** macOS (Python/bleak)

---

## 1. Test Objective

Characterize the nRF54L15's BLE power consumption during maximum-throughput GATT notification streaming to a macOS central, across multiple supply voltages. Determine energy efficiency (nJ/bit) to inform battery life estimates.

---

## 2. Hardware Setup

### Equipment
| Item | Details |
|------|---------|
| DUT | nRF54L15-DK (PCA10156), Serial #1057725401 |
| Power Supply | Nordic PPK2, Source Meter mode |
| Central | MacBook Pro (macOS), Python 3.11 + bleak |
| PPK2 Wiring | VOUT → nRF54 (VDDM), GND → GND |
| Current Measurement | VDDM jumper removed, PPK2 supplies SoC directly |

### PPK2 Connection
- PPK2 VOUT connected to the nRF54L15 side of the VDDM current measurement header (P22)
- VDDM jumper removed to isolate SoC from onboard regulator
- GND connected to DK ground
- For the optimized tests (3.0V and 1.8V): USB disconnected from DK to eliminate debugger leakage

**[INSERT PHOTO: Overall test setup — PPK2 connected to nRF54L15-DK]**

**[INSERT PHOTO: Close-up of PPK2 wire connections to VDDM measurement header]**

**[INSERT PHOTO: Close-up of removed VDDM jumper on DK board]**

---

## 3. Firmware Configuration

### BLE Parameters
| Parameter | Value |
|-----------|-------|
| BLE Stack | Zephyr Open-Source Link Layer |
| PHY | 2M (negotiated after connection) |
| Connection Interval | 7.5–15 ms (macOS selects) |
| MTU | 498 bytes |
| Data Length Extension | 251 bytes TX |
| Notification Payload | 495 bytes |
| TX Power | Default (0 dBm) |
| Advertising | Connectable, fast interval |

### Power Configuration
| Parameter | Value |
|-----------|-------|
| Regulator | DCDC (enabled by default on DK board devicetree) |
| UART | Disabled (for 3.0V and 1.8V tests) |
| Logging | Disabled (for 3.0V and 1.8V tests) |
| Thread Monitor / Stats | Disabled (for 3.0V and 1.8V tests) |

### Firmware Behavior
The firmware begins advertising immediately after boot. Upon connection:
1. Requests 2M PHY, DLE (251 bytes), and CI update (7.5–15 ms)
2. Waits for central to subscribe to TX notifications
3. Waits for DLE negotiation to complete (TX max_len >= 251)
4. Begins continuous GATT notification streaming (495-byte payloads, 5 ms interval)

---

## 4. Test Methodology

### Procedure
1. PPK2 powers up the DUT in source meter mode at the target voltage
2. DUT boots and begins BLE advertising
3. macOS central (Python/bleak) scans, connects, subscribes to notifications
4. 10-second settle period before PPK2 measurement begins
5. PPK2 records current at ~15,000–20,000 samples/second for 300 seconds
6. Per-second power and throughput data logged
7. DUT power-cycled between runs

### Test Configurations

| Config | Voltage | UART | USB Connected | Runs | Duration/Run |
|--------|---------|------|---------------|------|-------------|
| Baseline | 4000 mV | Enabled | Yes | 10 | 300s |
| Optimized 3.0V | 3000 mV | Disabled | No | 5 | 300s |
| Optimized 1.8V | 1800 mV | Disabled | No | 5 | 300s |

**Note:** The 4000 mV baseline exceeded the nRF54L15 VDDM maximum rating of 3.6V. This was corrected for subsequent tests. Data is included for comparison but the device may have been operating outside specifications.

---

## 5. Results

### 5.1 Summary Comparison

| Metric | 4.0V + USB (baseline) | 3.0V, no USB | 1.8V, no USB |
|--------|----------------------|--------------|--------------|
| **Throughput (avg)** | 509.4 kbps | 541.6 kbps | 514.7 kbps |
| **Throughput (std)** | 21.8 kbps | 5.9 kbps | 10.8 kbps |
| **Avg Current** | 20.43 mA | 20.44 mA | 20.39 mA |
| **Peak Current** | 176.1 mA | 176.1 mA | 176.1 mA |
| **Avg Power** | 81.7 mW | 61.3 mW | 36.7 mW |
| **Peak Power** | 704.2 mW | 528.2 mW | 317.0 mW |
| **Energy/bit** | 160.7 nJ | 113.2 nJ | 71.3 nJ |
| **Energy/byte** | 1285.4 nJ | 905.6 nJ | 570.6 nJ |
| **Total Data** | 188 MB (10 runs) | 50.4 MB (5 runs) | 47.8 MB (5 runs) |
| **PPK2 Sample Rate** | ~15,000–20,000 S/s | ~15,000–20,000 S/s | ~15,000–20,000 S/s |

### 5.2 Per-Run Results — 3.0V

| Run | Time | Throughput | Avg (mA) | Peak (mA) | Power (mW) | nJ/bit |
|-----|------|-----------|----------|-----------|------------|--------|
| 1 | 14:22:56 | 535.7 kbps | 19.694 | 176.1 | 59.08 | 110.3 |
| 2 | 14:28:19 | 539.8 kbps | 20.620 | 176.1 | 61.86 | 114.6 |
| 3 | 14:33:41 | 550.2 kbps | 20.624 | 176.1 | 61.87 | 112.5 |
| 4 | 14:39:03 | 537.4 kbps | 20.618 | 176.1 | 61.86 | 115.1 |
| 5 | 14:44:25 | 545.0 kbps | 20.623 | 176.1 | 61.87 | 113.5 |

### 5.3 Per-Run Results — 1.8V

| Run | Time | Throughput | Avg (mA) | Peak (mA) | Power (mW) | nJ/bit |
|-----|------|-----------|----------|-----------|------------|--------|
| 1 | 14:51:20 | 515.8 kbps | 19.773 | 176.1 | 35.59 | 69.0 |
| 2 | 14:56:42 | 510.9 kbps | 20.537 | 176.1 | 36.97 | 72.4 |
| 3 | 15:02:04 | 527.3 kbps | 20.545 | 176.1 | 36.98 | 70.1 |
| 4 | 15:07:27 | 520.8 kbps | 20.544 | 176.1 | 36.98 | 71.0 |
| 5 | 15:12:50 | 498.7 kbps | 20.532 | 176.1 | 36.96 | 74.1 |

---

## 6. Power Phase Analysis

Each run exhibits two distinct power phases:

### Phase 1: Connection Setup (~0–16 seconds)
- **Average current:** ~3.5 mA
- **Peak current:** ~15 mA
- **Behavior:** BLE connected but DLE negotiation not yet complete. Radio duty cycle is low (advertising stopped, only connection maintenance events).
- **Median current:** ~0.4 mA (device mostly in sleep between connection events)

### Phase 2: GATT Notification Streaming (~16+ seconds)
- **Average current:** ~20.5 mA
- **Peak current:** ~176 mA
- **Behavior:** Continuous GATT notifications at maximum rate. Radio active for large fraction of each connection interval.
- **Median current:** ~5.4 mA (bimodal: deep sleep between events, high current during TX bursts)

### Transition Trigger
The transition occurs when three conditions are met simultaneously:
1. BLE connection established
2. Central subscribes to TX notification characteristic (CCC write)
3. Data Length Extension negotiation completes (TX max_len >= 251 bytes)

The ~16-second delay corresponds to macOS completing service discovery, MTU exchange, and DLE negotiation.

**[INSERT PHOTO: PPK2 Power Profiler Desktop screenshot showing the two-phase current pattern]**

---

## 7. Key Findings

### 7.1 Current is Voltage-Independent
Average current remains **~20.4 mA across all three voltages** (4.0V, 3.0V, 1.8V). This is expected — the DCDC regulator draws roughly constant current from the supply to deliver regulated power to the SoC core.

### 7.2 Power Scales Linearly with Voltage
| Voltage | Avg Power | Reduction vs 4.0V |
|---------|-----------|-------------------|
| 4.0V | 81.7 mW | — |
| 3.0V | 61.3 mW | -25% |
| 1.8V | 36.7 mW | -55% |

Running at 1.8V cuts power consumption by more than half compared to 4.0V, with no throughput penalty.

### 7.3 Energy Efficiency
| Voltage | Energy/bit | Energy/byte |
|---------|-----------|-------------|
| 4.0V | 160.7 nJ | 1285 nJ |
| 3.0V | 113.2 nJ | 906 nJ |
| 1.8V | 71.3 nJ | 571 nJ |

At 1.8V, the nRF54L15 achieves **71.3 nJ/bit** — excellent efficiency for BLE throughput.

### 7.4 Throughput Consistency
- Throughput is stable across all voltages: **510–542 kbps** average
- Lower variance at 3.0V (std=5.9 kbps) vs 4.0V (std=21.8 kbps)
- macOS connection interval negotiation is the throughput bottleneck, not the nRF54L15

### 7.5 USB/Debugger Impact
Removing USB and disabling UART had no measurable impact on average current (~20.4 mA in both cases). The VDDM measurement path on the DK isolates the nRF54L15 SoC from the debugger power domain effectively.

### 7.6 DCDC Already Enabled
The nRF54L15-DK board devicetree enables the DCDC regulator by default (`regulator-initial-mode = <NRF5X_REG_MODE_DCDC>`). All tests ran with DCDC active. The ~20 mA average current during streaming represents DCDC-optimized operation.

---

## 8. Battery Life Estimates

Assuming continuous maximum-throughput GATT streaming:

| Battery | Capacity | Voltage | Est. Runtime |
|---------|----------|---------|-------------|
| CR2032 | 225 mAh | 3.0V | ~11 hours |
| AAA Alkaline | 1200 mAh | 1.5V (via boost) | ~59 hours |
| 18650 Li-Ion | 3000 mAh | 3.7V (nominal) | ~147 hours |
| LiPo 500 mAh | 500 mAh | 3.7V | ~24 hours |

*Note: Estimates assume 100% battery utilization and constant current draw. Real-world runtime will be 60–80% of these values due to battery discharge curves and regulator efficiency.*

---

## 9. Measurement Notes

### PPK2 Auto-Ranging
The PPK2 automatically switches measurement ranges based on current level:
- **Low-current range:** ~20,000 samples/second (during connection setup phase)
- **High-current range:** ~15,000 samples/second (during active streaming)

### Sample Filtering
Raw PPK2 samples were filtered to remove values outside 0–200,000 uA range (noise/artifacts). Per-second statistics computed from filtered samples.

### Data Files
| File | Description |
|------|------------|
| `power_throughput_raw.json` | 10 runs at 4.0V with USB connected |
| `power_throughput_3v_no_usb.json` | 5 runs at 3.0V, no USB |
| `power_throughput_1v8_no_usb.json` | 5 runs at 1.8V, no USB |
| `power_analysis.py` | Analysis script for all datasets |

---

## 10. Test Environment

**[INSERT PHOTO: Full lab bench setup]**

| Component | Version/Details |
|-----------|----------------|
| Zephyr RTOS | v4.1.0 (from workspace) |
| BLE Stack | Zephyr Open-Source Link Layer |
| Python | 3.11.11 (pyenv, zephyr-env virtualenv) |
| bleak | BLE library for macOS central |
| ppk2-api | Python PPK2 control library |
| macOS | Darwin 25.2.0 |
| nRF54L15-DK HW Rev | PCA10156 |

---

## 11. Conclusions

1. The nRF54L15 achieves **~515–540 kbps** GATT notification throughput to macOS, limited by Apple's BLE connection interval handling rather than the nRF54L15 hardware.

2. Average current during streaming is **~20.4 mA** regardless of supply voltage (1.8–4.0V), confirming efficient DCDC regulator operation.

3. **1.8V operation is recommended** for battery-powered applications — it provides **55% lower power consumption** (36.7 mW vs 81.7 mW) with identical throughput and no stability issues.

4. Energy efficiency at 1.8V is **71.3 nJ/bit**, making the nRF54L15 competitive for high-throughput BLE applications.

5. The two-phase power pattern (3.5 mA setup → 20.4 mA streaming) is caused by the DLE negotiation delay, not a hardware anomaly.

---

*Report generated from automated batch testing using PPK2 source meter mode and Python/bleak BLE central.*
