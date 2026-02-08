# nRF54L15 L2CAP CoC Throughput Test

BLE throughput test using L2CAP Connection-Oriented Channels (CoC) to bypass GATT/ATT overhead. Targets 700-900+ kbps vs ~540 kbps with GATT notifications.

## How It Works

1. Firmware advertises as `nRF54L15_L2CAP` and registers an L2CAP server with a dynamic PSM
2. A small GATT service exposes the PSM via a readable characteristic
3. The Python script connects, reads the PSM, and opens an L2CAP channel
4. Firmware streams 2000-byte SDUs continuously over the L2CAP channel
5. Python script reads from the channel's input stream and reports throughput

## Build & Flash

```bash
cd zephyr_workspace
source zephyrproject/zephyr/zephyr-env.sh

west build -b nrf54l15dk/nrf54l15/cpuapp nrf54l15_l2cap_test -p
west flash
```

## Python Setup

```bash
pip install pyobjc-framework-CoreBluetooth
```

## Run Test

```bash
# Default (run until Ctrl+C)
python3 l2cap_throughput_test.py

# Custom device name
python3 l2cap_throughput_test.py --name nRF54L15_L2CAP

# Fixed duration
python3 l2cap_throughput_test.py --duration 30
```

## Serial Monitor

Watch firmware logs:

```bash
python3 serial_monitor.py
```

## Expected Output

Firmware serial:
```
Starting nRF54L15 L2CAP CoC Throughput Test
Bluetooth initialized
L2CAP server registered, PSM=0x0080
Advertising started as 'nRF54L15_L2CAP'
Connected: XX:XX:XX:XX:XX:XX
PHY updated: TX=2, RX=2
Data Length updated: TX len=251 ...
L2CAP channel connected: tx.mtu=... rx.mtu=2000
TX: 128000 bytes total, 1024 kbps
```

Python:
```
Found nRF54L15_L2CAP (RSSI: -45)
Connected to nRF54L15_L2CAP
PSM = 0x0080 (128)
L2CAP channel opened!
RX: 850 kbps (avg: 820 kbps) | 102,400 bytes in 1.0s
```

## Troubleshooting

- **L2CAP channel fails to open**: macOS may require encryption. If this happens, the firmware `sec_level` can be bumped to `BT_SECURITY_L2` in `main.c`.
- **No data received**: Check serial monitor for "L2CAP channel connected" message. If it doesn't appear, the central may not be opening the channel on the correct PSM.
- **Low throughput**: Ensure 2M PHY and DLE are negotiated (check serial output). Connection interval of 7.5ms is ideal.
