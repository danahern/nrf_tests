# nRF54L15 BLE Throughput Testing - Session Summary

## Project Overview

BLE throughput testing on nRF54L15 DK (serial: 1057709871) using Zephyr RTOS v4.2.0.
Two firmware apps exist:
1. `nrf54l15_ble_test` - GATT notification throughput test
2. `nrf54l15_l2cap_test` - L2CAP CoC throughput test (current focus)

## Hardware & Environment

- **Board**: nRF54L15 DK (nrf54l15dk/nrf54l15/cpuapp)
- **Serial port**: `/dev/tty.usbmodem0010577098713` (UART console, 115200 baud)
- **BLE Controller**: Zephyr open-source LL (BT_LL_SW_SPLIT)
- **Central**: macOS (PyObjC CoreBluetooth)
- **Python**: pyenv `zephyr-env` (`/Users/danahern/.pyenv/versions/zephyr-env/bin/python3`)
- **Build from**: `/Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject/`

## Build & Flash Commands

```bash
cd /Users/danahern/code/claude/embedded/zephyr_workspace/zephyrproject
west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_l2cap_test -d ../nrf54l15_l2cap_test/build --pristine
west flash -d ../nrf54l15_l2cap_test/build
```

## Test Commands

```bash
cd /Users/danahern/code/claude/embedded/zephyr_workspace
/Users/danahern/.pyenv/versions/zephyr-env/bin/python3 serial_and_test.py
```

Or run the L2CAP test directly (with duration):
```bash
/Users/danahern/.pyenv/versions/zephyr-env/bin/python3 l2cap_throughput_test.py --duration 30
```

## Current Results

| Test | Steady-State | Average (30s) |
|------|-------------|---------------|
| GATT notifications (5ms sleep) | ~540 kbps | ~540 kbps |
| L2CAP CoC (495-byte SDU, sem) | ~517 kbps | ~480 kbps |

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

## Current Bottleneck

The Zephyr open-source LL controller (BT_LL_SW_SPLIT) only achieves ~40% of theoretical throughput. It doesn't pack enough packets into each 15ms connection event. Both GATT and L2CAP hit the same ~540 kbps ceiling.

## Next Steps to Reach 650+ kbps

1. **Switch to Nordic SoftDevice Controller (SDC)** - Requires installing nRF Connect SDK. The SDC is optimized for Nordic hardware and can pack more packets per connection event.
2. **Test with Linux central** - BlueZ may accept shorter CI (7.5ms) doubling throughput
3. **Test with iOS device** - Different BLE stack may negotiate better parameters
4. **Explore Zephyr LL advanced features** - CONFIG_BT_CTLR_ADVANCED_FEATURES may have tuning knobs
5. **Try nRF Connect SDK throughput sample** - Nordic's official sample achieves >1 Mbps with SDC

## File Inventory

- `nrf54l15_l2cap_test/src/main.c` - L2CAP CoC firmware
- `nrf54l15_l2cap_test/prj.conf` - Zephyr BLE config
- `l2cap_throughput_test.py` - PyObjC L2CAP RX throughput test
- `serial_and_test.py` - Combined serial monitor + test runner
- `serial_monitor.py` - Standalone serial monitor
- `nrf54l15_ble_test/` - Original GATT notification test
