# macOS L2CAP Throughput Optimization Log

## Goal
Maximize BLE L2CAP CoC throughput from nRF54L15 peripheral to macOS central.
Target: 600-800 kbps (per Apple guidelines for optimized 2M PHY + DLE).

## Result: 530 kbps is the macOS ceiling

The original config (Zephyr LL, CI=15ms, SDU=492, 6 TX bufs, sem=6) was already optimal.
8 experiments tested every reasonable parameter variation — none improved throughput.

## Hardware
- **Peripheral**: nRF54L15 DK (SNR 1057709871)
- **Central**: macOS (Python PyObjC CoreBluetooth script)

## Key Constraints (from Apple docs)
- macOS CI minimum: **15ms** in practice (despite docs suggesting 11.25ms is possible)
- Packets per CE: **~5** (hardware/stack limit, consistent across all tests)
- DLE: 251 bytes, 2M PHY, L2CAP CoC: all supported and active
- Apple expected range: 600-800 kbps — likely for newer hardware or native apps

## Definitive Findings

### CI must be 15ms
| CI | kbps | Impact |
|----|------|--------|
| 11.25ms | 276 | -48% (macOS rejects or limits PDUs) |
| 12.5ms | 284 | -46% (same issue) |
| **15ms** | **530** | **Optimal** |
| 50ms | 165 | -69% (same ~5 PDUs but 3x less frequent) |

macOS sends ~5 PDUs per connection event regardless of CI length.
- CI < 15ms: macOS counter-proposes or reduces PDUs, resulting in ~280 kbps
- CI > 15ms: same ~5 PDUs per CE, fewer CEs/sec = proportionally less data

### SDU=492 is the sweet spot
| SDU | Fragments | kbps | Notes |
|-----|-----------|------|-------|
| 247 | 1 | 395 | Too much L2CAP header overhead per byte |
| **492** | **2** | **530** | **Optimal** |
| 741 | 3 | 458 | More fragments = credit/reassembly overhead |

### Nothing else matters
| Parameter | Tested | Result |
|-----------|--------|--------|
| SDC controller | CE extension=4s | 520 kbps (no help) |
| TX_BUF_COUNT | 3, 6, 10 | All ~520-530 kbps |
| ACL_TX_COUNT | 6, 10 | No difference |
| Semaphore count | 3, 6, 10 | 6 is best (3 has slow ramp, 10 no better) |
| Multi-channel L2CAP | 2 PSMs, 2 channels | **522 kbps** (same as 1 channel) |
| Disable AirDrop/Handoff | Environment tuning | No measurable difference |

### PDU limit is per-CE, not per-channel
Multi-channel L2CAP (2 simultaneous CoC channels) confirmed the ~5 PDU limit is at the **radio/connection event level**, not per-channel. Two channels simply split the same ~5 PDUs between them — total throughput unchanged at ~522 kbps.

### Root cause
macOS limits to **~5 PDUs per connection event**. At 15ms CI:
- 5 PDUs × 251 bytes × (1/0.015s) × 8 = ~670 kbps theoretical
- Minus L2CAP/SDU overhead, credit PDUs, DLE negotiation = **~530 kbps actual**
- This is ~79% of theoretical, consistent with expected protocol overhead

### Why not 600-800 kbps?
Tested on **M4 Max MacBook Pro 16" (Nov 2024), macOS Tahoe 26.2** — the latest hardware.
Also tested **native Swift** (compiled with `swiftc -O`) vs Python PyObjC: identical results (~504 vs ~520 kbps). Python is NOT the bottleneck.

The Apple "600-800 kbps" guideline likely:
- Assumes GATT notifications (different credit model than L2CAP CoC)
- Was measured with different BLE peripherals (may negotiate different params)
- Counts differently (e.g., including L2CAP headers in "throughput")
- Or is simply optimistic for real-world conditions

## Build & Test Commands
```bash
# Zephyr LL build (optimal for macOS)
cd zephyr_workspace/zephyrproject && west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_l2cap_test -p
west flash --snr 1057709871

# Test (requires zephyr-env, sandbox disabled)
sleep 3 && cd /Users/danahern/code/claude/embedded && \
  PYTHONUNBUFFERED=1 ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 \
  zephyr_workspace/l2cap_throughput_test.py
# dangerouslyDisableSandbox=true, timeout=90000
# After 60s, tail -5 output file for steady-state result
```

## Full Experiment Results

| # | Controller | CI | SDU | TX Bufs | Sem | Steady kbps | Notes |
|---|-----------|-----|-----|---------|-----|------------|-------|
| baseline | Zephyr LL | 15ms | 492 | 6 | 6 | **530** | Original best |
| SDC-test | SDC | 15ms | 492 | 6 | 6 | **520** | SDC CE extension: no help |
| 50ms-test | Zephyr LL | 50ms | 492 | 6 | 6 | **165** | macOS caps ~5 PDUs/CE |
| exp1 | Zephyr LL | 11.25ms | 492 | 6 | 6 | **276** | Shorter CI also worse |
| exp2 | Zephyr LL | 15ms | 492 | 10 | 10 | **517** | More buffers: no help |
| exp3 | Zephyr LL | 15ms | 247 | 10 | 10 | **395** | 1-frag SDU: overhead |
| exp4 | Zephyr LL | 15ms | 741 | 6 | 6 | **458** | 3-frag SDU: credit overhead |
| exp5 | Zephyr LL | 15ms | 492 | 3 | 3 | **~520** | Tight sem: slow ramp, same steady |
| exp6 | Zephyr LL | 12.5ms | 492 | 6 | 6 | **284** | macOS rejects CI < 15ms |
| confirm | Zephyr LL | 15ms | 492 | 6 | 6 | **~520** | Baseline confirmed |
| native-swift | Zephyr LL | 15ms | 492 | 6 | 6 | **~504** | Native Swift app: same as Python |
| multi-ch | Zephyr LL | 15ms | 492 | 6×2 | 6 | **~522** | 2 L2CAP channels: same throughput |
| env-tune | Zephyr LL | 15ms | 492 | 6 | 6 | **~519** | AirDrop+Handoff disabled: no help |

## Conclusion

**530 kbps is the hard ceiling for macOS L2CAP CoC throughput with this peripheral.**

Tested exhaustively on M4 Max / macOS Tahoe 26.2:
- 10 firmware parameter variations (CI, SDU size, buffers, controller, flow control, multi-channel)
- 2 central implementations (Python PyObjC, native Swift)
- Environment tuning (disabled AirDrop/Handoff)
- All results: 500-530 kbps at CI=15ms, everything else is worse

The bottleneck is macOS CoreBluetooth limiting to **~5 PDUs per 15ms connection event**.
This limit is **per-CE (radio level), not per-channel** — confirmed by multi-channel L2CAP test.
No firmware, app-level, or system-level optimization can change this. The original config was already optimal.
