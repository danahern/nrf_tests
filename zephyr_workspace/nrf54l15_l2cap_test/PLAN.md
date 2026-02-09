# L2CAP CoC Throughput Test - Plan

## Status: Step 6 - nRF-to-nRF Test COMPLETE — 1285 kbps achieved

## Steps

1. **Project structure and config** - DONE
2. **Firmware (main.c)** - DONE
3. **Python test script** - DONE (15s scan timeout added)
4. **Documentation** - DONE
5. **Test and optimize L2CAP CoC** - BLOCKED BY macOS

## Current Best Config (flashed)

- SDU_LEN=492, TX_BUF_COUNT=6, sem flow control, stream priority 5
- Zephyr LL (BT_CTLR_PHY_2M, BT_CTLR_RX_BUFFERS=10)
- DLE gate, advertising stop on connect
- **nRF-to-nRF: 1285 kbps** (2.4x macOS, 2.9x iOS)
- **macOS: ~530 kbps** | **iOS: ~446 kbps**

## Important Notes

- **Device name**: Always `nRF54L15_Test`
- **Scan timeout**: 15s in l2cap_throughput_test.py
- **Zephyr LL builds**: `cd zephyr_workspace/zephyrproject && west build ...`
- **NCS/SDC builds**: `cd /opt/nordic/ncs/v3.2.1 && nrfutil sdk-manager toolchain launch ...`

## Optimization History

| Iter | SDU | Flow | Bufs | Controller | Steady kbps | Notes |
|------|-----|------|------|-----------|------------|-------|
| 5 | 495 | sem 6 | 6 tx | Zephyr LL | **540** | Previous best |
| 9 | 495 | sem 3 | 3 tx | Zephyr LL | 517 | DLE gate added |
| 10 | 495 | sem 3 | 3 tx | SDC 3pkt | 516 | SDC no better |
| 12 | 495 | sem 3 | 10 tx | SDC 10pkt | 495 | More LL bufs WORSE |
| 13 | 495 | sem 6 | 6 tx | Zephyr LL | 528 | DLE gate + adv stop |
| 14 | 492 | sem 6 | 6 tx | Zephyr LL | **530** | Fragment-aligned |
| 15 | 245 | sem 6 | 6 tx | Zephyr LL | 514 | 1-pkt SDU worse |
| 16 | 492 | sem 10 | 10 tx | Zephyr LL | 520 | Aggressive bufs worse |
| 17 | 492 | sem 6 | 6 tx, pri 1 | Zephyr LL | 493 | Hi priority starves BLE |
| 18 | 492 | sem 10 | ACL 502, 10 tx | Zephyr LL | 520 | Nordic-inspired, same |
| 19 | 492 | sem 6 | 6 tx | Zephyr LL (iOS central) | **446** | iPhone 17 Pro Max, WORSE than macOS |
| 20a | 492 | sem 6 | 6 tx | Zephyr LL (nRF central, recv) | **62** | 1-credit bottleneck |
| 20b | 492 | sem 6 | 6 tx | Zephyr LL (nRF central, seg_recv) | **1285** | 20x! seg_recv + bulk credits |

## Exhaustive Findings

### What we tried that DID NOT help:
- Nordic SoftDevice Controller (SDC) vs Zephyr LL → same or worse
- SDC TX_PACKET_COUNT=10 → worse (495 kbps)
- SDC MAX_CONN_EVENT_LEN=15000 → worse (429 kbps)
- ACL_TX_SIZE=502 → no improvement (DLE still 251)
- TX_BUF_COUNT=10 with aggressive buffering → worse
- Stream thread priority 1 → worse (starves BLE stack)
- 245-byte SDU (zero fragmentation) → worse (too much SDU overhead)
- 1251-byte SDU (large) → worse (too much fragmentation)
- Sleep-based flow control → worse than semaphore
- GATT notifications (various pacing) → same ceiling

### What DOES help:
- 492-495 byte SDU (2-fragment sweet spot)
- Semaphore flow control with 6 in-flight
- DLE gate (avoid 27-byte packet waste at start)
- Zephyr LL slightly better than SDC

### Root cause:
macOS forces 15ms CI and limits ~4-5 LL packets per connection event.
Nordic's official throughput sample achieves 1363 kbps using nRF-to-nRF (not macOS).
**The macOS BLE stack is the bottleneck, not the firmware.**

## nRF-to-nRF Central Test

### Central firmware: `zephyr_workspace/nrf54l15_l2cap_central/`

Protocol flow:
1. Scans for "nRF54L15_Test" → connects (CI 7.5-15ms negotiated)
2. Requests DLE (251) + 2M PHY
3. GATT discovers PSM → opens L2CAP CoC channel with `seg_recv` API
4. Gives 10 initial credits before connect + 10 more on connected
5. Replenishes 1 credit per received segment (continuous flow)
6. Prints throughput stats every 1s

### Hardware Assignment

| Role | Serial # | Board |
|------|----------|-------|
| Peripheral (TX) | 1057709871 | nrf54l15dk/nrf54l15/cpuapp |
| Central (RX) | 1057725401 | nrf54l15dk/nrf54l15/cpuapp |

### Test Results

**Iteration 20: nRF-to-nRF — 1285 kbps (SUCCESS)**

Connection parameters observed:
- CI: interval=12 (15.0ms) — peripheral negotiated, not 7.5ms
- DLE: TX=251, RX=251 ✓
- PHY: 2M ✓
- L2CAP: tx.mtu=492, tx.mps=247, rx.mtu=492, rx.mps=247

Throughput (70+ seconds, stable, no crash):
- Steady-state: **1280-1310 kbps**
- Average: **1285 kbps**
- Total: 11.7 MB in 73 seconds

Key fix: Switched from `recv`/`alloc_buf` (1 credit at a time) to `seg_recv` API
(`CONFIG_BT_L2CAP_SEG_RECV=y`) with 10 initial credits + 1 credit per segment replenish.
The default Zephyr L2CAP `recv` path gives only 1 initial credit and replenishes 1 per SDU,
creating a credit round-trip bottleneck (~62 kbps).

### Optimization History (all centrals)

| Iter | Central | Steady kbps | Notes |
|------|---------|------------|-------|
| 14 | macOS (Python) | **530** | Best macOS result, fragment-aligned |
| 19 | iOS (iPhone 17 PM) | **446** | Worse than macOS |
| 20a | nRF54L15 DK (recv) | **62** | 1-credit bottleneck, crashed after 3s |
| 20b | nRF54L15 DK (seg_recv) | **1285** | 20x improvement, stable |
| 21a | nRF54L15 DK (CI=7.5ms) | ~1250 | Crashes after ~3s — controller can't sustain |
| 21b | nRF54L15 DK (CI=10ms) | ~1180 | Crashes after ~3s — same issue |
| 21c | nRF54L15 DK (CI=10ms, initial) | ~1180 | Crashes after ~3s — not CI change, fundamental limit |

### CI Experiment Results

Tested CI values below 15ms to find max throughput. **All CI < 15ms crash the central after ~3s.**

| CI (ms) | Interval | Steady kbps | Stable? | Notes |
|---------|----------|------------|---------|-------|
| 15.0 | 12 | **1285** | YES (70s+) | Best overall — optimal |
| 10.0 | 8 | ~1180 | NO (3s) | Lower throughput AND crashes |
| 7.5 | 6 | ~1250 | NO (3s) | Reason 62 at connect or crash after 3s |

**Conclusion**: 15ms CI is the sweet spot. Shorter CI means shorter connection events,
fitting fewer packets per CE, so throughput actually decreases. The Zephyr LL controller
also hard-resets after ~3s of sustained data at CI < 15ms (likely a controller scheduling
or buffer issue). **1285 kbps at 15ms CI is the maximum stable nRF-to-nRF throughput.**

### Build & Flash Commands

```bash
# Build central
cd zephyr_workspace/zephyrproject
west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_l2cap_central -p

# Flash central to DK #1057725401
west flash --snr 1057725401

# Reset peripheral DK #1057709871
nrfutil device reset --serial-number 1057709871
```
