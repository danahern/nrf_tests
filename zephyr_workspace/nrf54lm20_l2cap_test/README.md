# nRF54LM20 L2CAP CoC Throughput Test

Streams data over L2CAP Connection-Oriented Channel (492-byte SDUs). A GATT service exposes the dynamically allocated PSM for central discovery. Ported from the proven nrf54l15_l2cap_test.

## Build

```bash
cd zephyr_workspace/zephyrproject
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_l2cap_test -d ../nrf54lm20_l2cap_test/build -p
```

## Central Receiver

```bash
cd power_comparison
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 ble_central.py --mode l2cap --name nRF54LM20_Test
```

## Verified Results

- PSM 0x0080 registered and discoverable via GATT
- L2CAP channel opens successfully from macOS CoreBluetooth
- ~547 kbps peak, 434 kbps average to macOS
