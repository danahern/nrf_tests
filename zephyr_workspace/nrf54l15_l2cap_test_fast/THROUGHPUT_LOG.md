# nRF54L15 L2CAP CoC Throughput Optimization Log

## Test Setup
- **Hardware**: 2x nRF54L15 DK (peripheral SNR 1057709871, central SNR 1057725401)
- **PHY**: 2M throughout
- **DLE**: 251 bytes TX/RX throughout
- **L2CAP**: Connection-Oriented Channel, seg_recv on central
- **Controller**: Nordic SoftDevice Controller (SDC) unless noted

## Baseline (Zephyr LL)
| CI | Throughput | Notes |
|----|-----------|-------|
| 15ms | 1285 kbps | Zephyr open-source LL, crashes at CI < 15ms |

## Experiment Results

| # | CI | Credits | Credit Batch | SDU | Periph SDC TX | Central SDC RX | Throughput | Stable? | Notes |
|---|-----|---------|-------------|-----|---------------|----------------|-----------|---------|-------|
| 0 | 7.5ms | 10 | 1-per-seg | 492 | 10 | 10 | **1050 kbps** | Yes | Initial SDC migration |
| 1A | 15ms | 10 | 1-per-seg | 492 | 10 | 10 | **1180 kbps** | Yes | Fixed peripheral CI override |
| 1B | 15ms | 10 | 1-per-seg | 492 | 20 | 10 | **1179 kbps** | Yes | More LL buffers: no help |
| 1C | 15ms | 10 | 1-per-seg | 2000 | 20 | 10 | **1183 kbps** | Yes | Larger SDU: marginal +4 kbps |
| 2 | 15ms | 40 | 10-per-10 | 2000 | 20 | 10 | **1200 kbps** | Yes | Batch credits: +20 kbps |
| 3 | 7.5ms | 80 | 10-per-10 | 2000 | 20 | 10 | **1066 kbps** | Yes | 7.5ms worst with SDC |
| 4b | 30ms | 80 | 10-per-10 | 2000 | 20 | 10 | **1265 kbps** | Yes | Longer CI = more PDUs/event |
| 5 | 50ms | 80 | 10-per-10 | 2000 | 20 | 10 | **1317 kbps** | Yes | Very stable |
| 6 | 100ms | 80 | 10-per-10 | 2000 | 20 | 20 | **1340 kbps** | Mixed | High but some variance |
| 7 | 200ms | 80 | 10-per-10 | 2000 | 20 | 20 | **1351 kbps** | Bursty | Periodic dips (host can't keep up) |
| 8 | 75ms | 80 | 10-per-10 | 2000 | 20 | 20 | **1333 kbps** | Yes | Between 50ms and 100ms |
| 9 | 100ms | 80 | 20-per-20 | 2000 | 20 | 20 | **1271 kbps** | Dips | Large credit batches: starvation |
| 10 | 100ms | 80 | 10-per-10 | 2000 | 20 | 20 | **~1230 kbps** | Dips | SDC_RX=20 hurts stability |
| 11 | 50ms | 80 | 10-per-10 | 2000 | 20 | **10** | **1317 kbps** | Rock solid | **Best overall config** |

## Key Findings

### 1. SDC throughput scales with connection interval (up to a point)
```
CI vs Throughput (SDC, 2M PHY, DLE=251):
  7.5ms:  1050 kbps  (worst - too many CE transitions)
  15ms:   1200 kbps
  30ms:   1265 kbps
  50ms:   1317 kbps  ← optimal (stable + high)
  75ms:   1333 kbps
  100ms:  1340 kbps  (highest stable, but host buffer pressure)
  200ms:  1351 kbps  (peak, but bursty/unreliable)
```

### 2. Excess LL buffers can hurt
`SDC_RX_PACKET_COUNT=20` on the central caused throughput variance vs `=10`. The extra buffers likely create memory pressure or scheduling overhead in the SDC.

### 3. L2CAP credit strategy matters
- 1-per-segment: ~1180 kbps (credit PDUs waste airtime)
- 10-per-10 batch: ~1200+ kbps (reduces overhead)
- 20-per-20 batch: causes starvation dips (too few credits in-flight between replenishments)
- 80 initial credits + 10-batch is the sweet spot

### 4. Buffer sizes have diminishing returns
Beyond baseline levels, adding host or LL buffers doesn't improve throughput. Airtime is the bottleneck.

### 5. Theoretical maximum
At 2M PHY, DLE=251:
- Per PDU round-trip: 1048us (TX) + 150us (IFS) + 44us (ACK) + 150us (IFS) = 1392us
- At 50ms CI: 50000/1392 = 35 PDUs/event × 251 bytes × 8 / 0.05s = **1426 kbps theoretical**
- Achieved: 1317 kbps = **92% efficiency**
- Remaining 8% = CE open/close overhead + credit PDU airtime + host scheduling

## Recommended Configuration (50ms CI, 1317 kbps)

### Why 50ms over 100ms?
- 50ms: 1317 kbps, rock-solid (1315-1319 range, zero dips in 35s test)
- 100ms: 1340 kbps potential but variable (1008-1340 range, periodic dips)
- 50ms is only 23 kbps less but vastly more consistent

### Peripheral prj.conf key settings:
```
CONFIG_BT_LL_SOFTDEVICE=y
CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT=4000000
CONFIG_BT_CTLR_SDC_TX_PACKET_COUNT=20
CONFIG_BT_CTLR_SDC_RX_PACKET_COUNT=10
CONFIG_BT_PERIPHERAL_PREF_MIN_INT=40   # 50ms
CONFIG_BT_PERIPHERAL_PREF_MAX_INT=40
CONFIG_BT_L2CAP_TX_BUF_COUNT=10
CONFIG_BT_BUF_ACL_TX_COUNT=10
SDU_LEN=2000, TX_BUF_COUNT=10
```

### Central key settings:
```
CONFIG_BT_LL_SOFTDEVICE=y
CONFIG_BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT=4000000
CONFIG_BT_CTLR_SDC_TX_PACKET_COUNT=10
CONFIG_BT_CTLR_SDC_RX_PACKET_COUNT=10
interval_min/max = 40 (50ms in main.c)
INITIAL_CREDITS = 80
SDU_LEN=2000, RX_MPS=247
credit batch = 10 per 10 segments
```
