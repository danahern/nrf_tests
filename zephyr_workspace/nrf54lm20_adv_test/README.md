# nRF54LM20 BLE Advertising Power Test

Non-connectable BLE advertising at 1-second interval. No connections accepted. Measures power consumption of the BLE radio in advertising mode.

## Build

```bash
cd zephyr_workspace/zephyrproject
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_adv_test -d ../nrf54lm20_adv_test/build -p
```

## Verified

- BLE advertising visible as "nRF54LM20_Test" in BLE scanner
- BT 5.4, nRF54Lx variant, Standard Bluetooth controller
