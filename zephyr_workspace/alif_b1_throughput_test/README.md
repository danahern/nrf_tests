# Alif B1 GATT Notification Throughput Test

Streams continuous 244-byte GATT notifications using the Alif ROM-based BLE stack. Uses NUS-compatible UUIDs for interoperability with the same BLE central receiver as the nRF tests.

## Build

```bash
cd sdk-alif
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_throughput_test -d ../zephyr_workspace/alif_b1_throughput_test/build -p
```

## Central Receiver

Same as nRF:
```bash
cd power_comparison
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 ble_central.py --mode gatt --name Alif_B1_Test
```

## Notes

- Uses `gatt_srv_event_send()` with `GATT_NOTIFY` (not `bt_gatt_notify()`)
- NUS TX UUID: 6e400003-b5a3-f393-e0a9-e50e24dcca9e (same as nRF)
- Notification sent one at a time (`ntf_ongoing` flag for flow control)
