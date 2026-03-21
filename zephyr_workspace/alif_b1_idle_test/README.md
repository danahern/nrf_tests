# Alif B1 Idle Power Test

Minimal firmware for baseline power measurement. Enters idle loop with 1-second periodic wakeup.

## Build

Requires the Alif Zephyr SDK (`sdk-alif/`) and `gnuarmemb` toolchain:

```bash
cd sdk-alif
GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
  west build -b alif_b1_dk/ab1c1f4m51820hh0/rtss_he \
  ../zephyr_workspace/alif_b1_idle_test -d ../zephyr_workspace/alif_b1_idle_test/build -p
```

## Flash

Via SETOOLS (requires SE-UART communication with Secure Enclave):
```bash
export ALIF_SE_TOOLS_DIR=/path/to/alif_security_tools/app-release-exec-linux
west flash -d ../zephyr_workspace/alif_b1_idle_test/build
```

## Notes

- Does not use Alif deep PM (STOP mode) yet — requires RTC0 DTS overlay from `le_periph_pm` sample
- Target: 700 nA in STOP mode (per Alif B1 datasheet)
- Board operates at 1.8V (VDD_MAIN via JP4)
