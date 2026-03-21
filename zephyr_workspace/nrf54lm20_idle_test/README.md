# nRF54LM20 Idle Power Test

Minimal firmware that enters deepest sleep with periodic 1-second wakeup. All peripherals disabled for baseline power measurement.

## Build

```bash
cd zephyr_workspace/zephyrproject
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_idle_test -d ../nrf54lm20_idle_test/build -p
```

Debug build (UART enabled):
```bash
west build -b nrf54lm20dk/nrf54lm20a/cpuapp ../nrf54lm20_idle_test -d ../nrf54lm20_idle_test/build_debug -p -- -DOVERLAY_CONFIG=debug.conf
```

## Flash

```bash
nrfutil device program --firmware nrf54lm20_idle_test/build/zephyr/zephyr.hex --serial-number 1051849098
```

## Expected Behavior

- No UART output (power build) or boot banner only (debug build)
- Device sleeps for 1s, wakes briefly, sleeps again
- Target: sub-milliamp average current
