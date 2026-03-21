# nRF54LM20 GATT Notification Throughput Test

Streams continuous GATT notifications via NUS TX characteristic (495-byte payloads, MTU 498). Measures power during active BLE data transfer.

## Build

```bash
cd zephyr_workspace/zephyrproject
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_throughput_test -d ../nrf54lm20_throughput_test/build -p
```

## Central Receiver

```bash
cd power_comparison
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 ble_central.py --mode gatt --name nRF54LM20_Test
```

## Verified Results

- MTU negotiated to 498
- ~530 kbps peak, 460 kbps average to macOS
- DLE and 2M PHY negotiated successfully
