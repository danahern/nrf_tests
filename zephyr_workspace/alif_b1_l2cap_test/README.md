# Alif B1 L2CAP CoC Throughput Test

L2CAP Connection-Oriented Channel server using the Alif ROM-based BLE stack. Registers SPSM 0x0080, accepts CoC connections, and streams 492-byte SDUs. Includes a GATT PSM discovery service (same UUIDs as nRF L2CAP test) for central interoperability.

## Build

```bash
cd sdk-alif
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_l2cap_test -d ../zephyr_workspace/alif_b1_l2cap_test/build -p
```

## Central Receiver

Same as nRF:
```bash
cd power_comparison
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 ble_central.py --mode l2cap --name Alif_B1_Test
```

## Notes

- Uses `l2cap_coc_spsm_add()` / `l2cap_chan_sdu_send()` (Alif ROM L2CAP API)
- PSM discovery service UUID: 12345678-1234-5678-1234-56789ABCDEF0 (same as nRF)
- SDU sent one at a time with `sdu_pending` flag
