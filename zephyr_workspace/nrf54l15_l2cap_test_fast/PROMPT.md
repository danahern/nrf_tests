# nRF54L15 BLE Throughput Testing - Session Summary

## Project Overview

BLE throughput testing on nRF54L15 DK (serial: 1057709871).
Two firmware apps exist:
1. `nrf54l15_ble_test` - GATT notification throughput test
2. `nrf54l15_l2cap_test` - L2CAP CoC throughput test (current focus)

**Device Name**: Both apps advertise as `nRF54L15_Test` (important: always use this name).

## Hardware & Environment

- **Board**: nRF54L15 DK (nrf54l15dk/nrf54l15/cpuapp)
- **Serial port**: `/dev/tty.usbmodem0010577098713` (UART console, 115200 baud)
- **BLE Controllers tested**: Zephyr open-source LL (BT_LL_SW_SPLIT) and Nordic SoftDevice Controller (SDC)
- **Central**: macOS (PyObjC CoreBluetooth for L2CAP, bleak for GATT)
- **Python**: pyenv `zephyr-env` (`/Users/danahern/.pyenv/versions/zephyr-env/bin/python3`)
- **NCS**: v3.2.1 at `/opt/nordic/ncs/v3.2.1/`

## Build & Flash Commands

### NCS/SDC (current, preferred):
```bash
cd /opt/nordic/ncs/v3.2.1
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- west build -b nrf54l15dk/nrf54l15/cpuapp /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_l2cap_test -d /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_l2cap_test/build --pristine
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- west flash -d /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_l2cap_test/build
```

### GATT test with NCS:
```bash
cd /opt/nordic/ncs/v3.2.1
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- west build -b nrf54l15dk/nrf54l15/cpuapp /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_ble_test -d /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_ble_test/build_ncs --pristine -- -DCONF_FILE=prj_ncs.conf
nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- west flash -d /Users/danahern/code/claude/embedded/zephyr_workspace/nrf54l15_ble_test/build_ncs
```

### Zephyr (legacy):
```bash
cd /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject
west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_l2cap_test -d ../nrf54l15_l2cap_test/build --pristine
west flash -d ../nrf54l15_l2cap_test/build
```

## Test Commands

```bash
cd /Users/danahern/code/claude/embedded/zephyr_workspace
# Combined serial + L2CAP test (30s):
/Users/danahern/.pyenv/versions/zephyr-env/bin/python3 serial_and_test.py
# L2CAP test only:
/Users/danahern/.pyenv/versions/zephyr-env/bin/python3 l2cap_throughput_test.py --duration 30
# GATT test:
/Users/danahern/.pyenv/versions/zephyr-env/bin/python3 ble_throughput_test.py --mac-tx 0 --name nRF54L15_Test
```

## Current Results

| Config | Steady-State | Notes |
|--------|-------------|-------|
| GATT + Zephyr LL | ~540 kbps | 5ms sleep-based pacing |
| GATT + SDC | ~508 kbps | Same 5ms sleep pacing |
| L2CAP + Zephyr LL | ~520 kbps | 495-byte SDU, semaphore flow |
| L2CAP + SDC (3 TX pkts) | ~516 kbps | Default SDC TX_PACKET_COUNT |
| L2CAP + SDC (10 TX pkts) | untested | BT_CTLR_SDC_TX_PACKET_COUNT=10 |

## Negotiated BLE Parameters (macOS)

- PHY: 2M
- DLE: TX=251, RX=251
- Connection Interval: 15ms (requested 7.5-15ms, macOS gives 15ms)
- L2CAP: tx.mtu=1251, tx.mps=1251, rx.mtu=495, rx.mps=247

## Key Issues Fixed

1. **err -122 (ENOSPC)**: Added `alloc_buf` callback for RX segmentation; dynamic SDU size capped at tx.mtu
2. **PyObjC stream.read_maxLength_ returns tuple**: Handle `(bytes_read, data)` tuple
3. **DLE ramp-up waste**: Gate streaming on `dle_ready` flag
4. **Advertising during data transfer**: Stop advertising after connection
5. **PHY update fails with SDC (err -5)**: SDC auto-updates PHY, removed manual call

## Critical Notes for Future Sessions

- **Device name is always `nRF54L15_Test`** for both GATT and L2CAP firmware
- **L2CAP test script has 15s scan timeout** to prevent hanging. If device not found, script exits.
- **Never run BLE scan commands without a timeout** - macOS CoreBluetooth can hang indefinitely
- **When switching between GATT and L2CAP firmware**, reset the device and wait 5s for macOS BLE cache
- **NCS builds must be run from `/opt/nordic/ncs/v3.2.1`** with `nrfutil sdk-manager toolchain launch`
- **GATT NCS build uses `prj_ncs.conf`** (pass `-DCONF_FILE=prj_ncs.conf`), L2CAP uses default `prj.conf`
- **BT_CTLR_PHY_2M and BT_CTLR_RX_BUFFERS are Zephyr LL-only** - not available with SDC

## SDC Throughput Tuning Knobs

Key Kconfig options in `/opt/nordic/ncs/v3.2.1/nrf/subsys/bluetooth/controller/Kconfig`:
- `BT_CTLR_SDC_TX_PACKET_COUNT` (default 3, range 1-20): LL ACL TX buffers per connection. **Key throughput knob.**
- `BT_CTLR_SDC_RX_PACKET_COUNT` (default 2, range 1-20): LL ACL RX buffers per connection
- `BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT` (default 7500us): Time reserved for connections per CI. Tried 15000 - made things worse!
- `BT_CTLR_SDC_CONN_EVENT_EXTEND_DEFAULT` (default y): Extends CE if more data available
- `BT_CTLR_SDC_LLPM`: Nordic proprietary low-latency packet mode (CI down to 1ms)

## Current Firmware State (L2CAP)

- SDU_LEN=495, TX_BUF_COUNT=3 (semaphore flow control)
- Stream thread priority 5, stats thread priority 7
- DLE-ready gate, advertising stop on connect
- prj.conf: BT_CTLR_SDC_TX_PACKET_COUNT=10, BT_BUF_ACL_TX_COUNT=10

## Bottleneck Analysis

Both Zephyr LL and SDC give identical ~500-520 kbps with macOS. The ceiling is macOS's BLE stack:
- Forces 15ms CI (won't accept 7.5ms)
- Controls connection event length and L2CAP credit flow
- Theoretical max at 15ms CI, 2M PHY, 251 DLE: ~1.3 Mbps (we get ~40%)

## File Inventory

- `nrf54l15_l2cap_test/src/main.c` - L2CAP CoC firmware
- `nrf54l15_l2cap_test/prj.conf` - BLE config (NCS/SDC compatible)
- `nrf54l15_ble_test/src/main.c` - GATT notification firmware
- `nrf54l15_ble_test/prj_ncs.conf` - NCS-specific GATT config
- `l2cap_throughput_test.py` - PyObjC L2CAP RX throughput test (15s scan timeout)
- `ble_throughput_test.py` - bleak GATT throughput test
- `serial_and_test.py` - Combined serial monitor + L2CAP test runner
