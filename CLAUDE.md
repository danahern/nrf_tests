# Project: Embedded BLE Testing (nRF54L15)

## Python Environment

Use the `zephyr-env` pyenv virtualenv for scripts that need PyObjC (CoreBluetooth, Foundation):
```bash
~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3
```
The default system Python (3.13) does not have PyObjC installed.

## Serial Port & Bluetooth Rules

**NEVER access serial ports directly** (e.g., `cat /dev/tty.*`, `screen`, `stty`, `picocom`). Serial ports are shared resources — opening them from here conflicts with other tools (minicom, serial monitors, etc.) and can lock the port.

Use `python3 zephyr_workspace/serial_monitor.py` to read serial output — it handles port access safely, resets the device, and captures 60s of logs. For just resetting, `nrfutil device reset --serial-number 1057709871` is fine.

**NOTE:** Both the serial monitor and the BLE throughput test (`l2cap_throughput_test.py`) require hardware access (serial ports / Bluetooth) that is not available from Claude's sandbox. Ask the user to run these from their terminal and share the output.

## Current State

### Throughput Results
- **nRF-to-nRF (SDC, 50ms CI)**: **1317 kbps** — best overall (92% theoretical max)
- **nRF-to-nRF (Zephyr LL, 15ms CI)**: 1285 kbps — crashes at CI < 15ms
- **macOS (Python, 15ms CI)**: ~530 kbps — Apple CI bottleneck
- **iOS (Swift app, 15ms CI)**: ~446 kbps — Apple CI + app overhead

### Current Experiment
Testing whether macOS accepts 50ms CI from peripheral (should increase throughput).
Peripheral firmware (`nrf54l15_l2cap_test`) flashed with CI=50ms preference.

### Key Files
- Peripheral (Zephyr LL, for macOS/iOS): `zephyr_workspace/nrf54l15_l2cap_test/`
- Peripheral (SDC, for nRF-to-nRF): `zephyr_workspace/nrf54l15_l2cap_test_fast/`
- Central (SDC, nRF-to-nRF): `zephyr_workspace/nrf54l15_l2cap_central_fast/`
- macOS test: `zephyr_workspace/l2cap_throughput_test.py`
- iOS app: `zephyr_workspace/L2CAPTest/`
- Experiment log: `zephyr_workspace/nrf54l15_l2cap_test/PLAN.md`
- SDC optimization log: `zephyr_workspace/nrf54l15_l2cap_test_fast/THROUGHPUT_LOG.md`

### Hardware Assignment
| Role | Serial # |
|------|----------|
| Peripheral (TX) | 1057709871 |
| Central (RX) | 1057725401 |

### Build Commands
- **Zephyr LL builds**: `cd zephyr_workspace/zephyrproject && west build -b nrf54l15dk/nrf54l15/cpuapp ../nrf54l15_l2cap_test -p`
- **NCS/SDC builds**: `cd /opt/nordic/ncs/v3.2.1 && nrfutil sdk-manager toolchain launch --ncs-version v3.2.1 -- west build ...`
