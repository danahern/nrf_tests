# Alif B1 BLE Advertising Power Test

Non-connectable BLE advertising at 1-second interval using the Alif ROM-based BLE stack. Uses the `alif_ble.h` / `gapm` API (not standard Zephyr BLE).

## Build

```bash
cd sdk-alif
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_adv_test -d ../zephyr_workspace/alif_b1_adv_test/build -p
```

## Notes

- Advertises as "Alif_B1_Test"
- Rejects incoming connections (advertising-only power measurement)
- BLE controller is ROM-based — uses `gapm_le_create_adv_legacy()`, not `bt_le_adv_start()`
