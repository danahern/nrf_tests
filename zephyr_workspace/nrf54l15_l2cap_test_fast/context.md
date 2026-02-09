# L2CAP CoC Throughput Test - Technical Context

## Why L2CAP CoC over GATT?

The GATT-based test (`nrf54l15_ble_test`) achieves ~540 kbps. The bottleneck is:
- macOS enforces 15ms connection interval (requested 7.5ms)
- GATT/ATT adds 3-byte header per notification
- ATT flow control limits in-flight notifications

L2CAP CoC eliminates ATT overhead, uses credit-based flow control, and supports large SDUs (2000 bytes) that are automatically segmented into link-layer PDUs.

## Architecture

### Firmware (`src/main.c`)
- **L2CAP server**: Registered with `bt_l2cap_server_register()`, PSM=0 (dynamic allocation)
- **PSM discovery**: Minimal GATT service with one readable characteristic (uint16 PSM)
- **TX flow control**: Semaphore-based double buffering. `sent` callback releases semaphore, stream thread waits on it before sending next SDU
- **Buffer pool**: `NET_BUF_POOL_DEFINE` with `BT_L2CAP_SDU_BUF_SIZE(2000)`, 2 buffers

### Python (`l2cap_throughput_test.py`)
- Uses PyObjC CoreBluetooth directly (bleak doesn't support L2CAP CoC)
- Runs on CFRunLoop (required by CoreBluetooth)
- NSStreamDelegate receives `NSStreamEventHasBytesAvailable` events
- NSTimer fires every 1s for stats

## Key API Details

### `bt_l2cap_chan_send()`
- Stack takes ownership of the net_buf
- Must reserve `BT_L2CAP_SDU_CHAN_SEND_RESERVE` bytes of headroom
- Returns 0 on success, negative errno on failure
- `sent` callback fires when the SDU transmission completes

### CoreBluetooth L2CAP
- `peripheral.openL2CAPChannel_(psm)` triggers `peripheral:didOpenL2CAPChannel:error:`
- Channel provides `inputStream()` and `outputStream()`
- Input stream must be scheduled on a run loop and opened before data flows

## Decisions

- **SDU size 2000**: Large enough to amortize protocol overhead, small enough to avoid buffer pressure
- **Double buffering (2 TX bufs)**: Keeps the link saturated without excessive memory usage
- **Security L1**: No encryption required. If macOS demands it, bump to L2
- **Stream thread priority 5**: Higher than stats (7) to prioritize throughput
