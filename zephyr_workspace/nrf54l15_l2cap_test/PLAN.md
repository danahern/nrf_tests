# L2CAP CoC Throughput Test - Plan

## Status: Step 7 - SDC Optimization COMPLETE — 1317 kbps achieved

## Steps

1. **Project structure and config** - DONE
2. **Firmware (main.c)** - DONE
3. **Python test script** - DONE (15s scan timeout added)
4. **Documentation** - DONE
5. **Test and optimize L2CAP CoC** - BLOCKED BY macOS

## Current Best Config (flashed — `_fast` variants)

- SDU_LEN=2000, TX_BUF_COUNT=10, batch credit flow (80 initial, 10-per-10)
- Nordic SoftDevice Controller (SDC), 2M PHY, DLE=251
- CI=50ms (interval 40), SDC_TX=20 (periph), SDC_TX/RX=10 (central)
- **nRF-to-nRF: 1317 kbps** (92% of 1426 kbps theoretical max)
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
| SDC-0 | 2000 | batch 10/10 | 10 tx | SDC, CI=7.5ms | **1050** | Initial SDC migration |
| SDC-5 | 2000 | batch 10/10 | 10 tx | SDC, CI=50ms | **1317** | Best overall — 92% theoretical max |
| SDC-11 | 2000 | batch 10/10 | 10 tx | SDC, CI=50ms, RX=10 | **1317** | Rock solid, recommended config |

## Exhaustive Findings

### What we tried that DID NOT help (macOS central):
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

### What DOES help (macOS central):
- 492-495 byte SDU (2-fragment sweet spot)
- Semaphore flow control with 6 in-flight
- DLE gate (avoid 27-byte packet waste at start)
- Zephyr LL slightly better than SDC

### macOS root cause:
macOS forces 15ms CI and limits ~4-5 LL packets per connection event.
**The macOS BLE stack is the bottleneck, not the firmware.**

### What DOES help (nRF-to-nRF, SDC optimization):
- SDC controller (stable at all CI values, unlike Zephyr LL which crashes at CI < 15ms)
- 50ms CI (longer CI = more PDUs per connection event = higher throughput)
- SDU_LEN=2000 (marginal gain, reduces SDU header overhead)
- Batch credit replenishment (10-per-10 vs 1-per-segment reduces credit PDU airtime)
- 80 initial L2CAP credits (ensures sender never stalls at startup)
- SDC_TX_PACKET_COUNT=20 on peripheral (enough LL buffers to fill a 50ms CE)
- SDC_RX_PACKET_COUNT=10 on central (more than 10 hurts stability)

### What DID NOT help (nRF-to-nRF, SDC):
- CI < 15ms with SDC → lower throughput (shorter CE = fewer PDUs)
- CI > 100ms → diminishing returns + bursty behavior
- SDC_RX_PACKET_COUNT=20 on central → throughput variance
- 20-per-20 credit batch → credit starvation dips
- Extra LL TX buffers on central → no improvement

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
| SDC-0 | nRF54L15 DK (SDC, CI=7.5ms) | **1050** | SDC stable but short CI = low throughput |
| SDC-5 | nRF54L15 DK (SDC, CI=50ms) | **1317** | Optimal — 92% of theoretical max |
| SDC-6 | nRF54L15 DK (SDC, CI=100ms) | **1340** | Higher peak but variable |
| SDC-7 | nRF54L15 DK (SDC, CI=200ms) | **1351** | Highest peak but bursty |
| SDC-11 | nRF54L15 DK (SDC, CI=50ms, tuned) | **1317** | Rock solid — best overall |

### CI Experiment Results

Tested CI values below 15ms to find max throughput. **All CI < 15ms crash the central after ~3s.**

| CI (ms) | Interval | Steady kbps | Stable? | Notes |
|---------|----------|------------|---------|-------|
| 15.0 | 12 | **1285** | YES (70s+) | Best overall — optimal |
| 10.0 | 8 | ~1180 | NO (3s) | Lower throughput AND crashes |
| 7.5 | 6 | ~1250 | NO (3s) | Reason 62 at connect or crash after 3s |

**Zephyr LL Conclusion**: 15ms CI is the Zephyr LL sweet spot. CI < 15ms crashes after ~3s.

**SDC Conclusion**: Switching to Nordic SoftDevice Controller (SDC) enables longer CI
values (50ms+) that pack more PDUs per connection event. SDC at 50ms CI achieves
**1317 kbps** — a 2.5% improvement over Zephyr LL, with rock-solid stability.
See `nrf54l15_l2cap_test_fast/THROUGHPUT_LOG.md` for full SDC optimization log.

### Build & Flash Commands

Zephyr LL builds (original `_test` and `_central`):
```bash
cd zephyr_workspace/zephyrproject
west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_l2cap_central -p
west flash --snr 1057725401
```

SDC builds (`_test_fast` and `_central_fast`, require NCS):
```bash
cd /opt/nordic/ncs/v3.2.1

# Build & flash peripheral
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- \
  west build -b nrf54l15dk/nrf54l15/cpuapp \
  /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_l2cap_test_fast \
  -d build_periph -p
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- \
  west flash -d build_periph --snr 1057709871

# Build & flash central
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- \
  west build -b nrf54l15dk/nrf54l15/cpuapp \
  /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_l2cap_central_fast \
  -d build -p
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- \
  west flash -d build --snr 1057725401
```
