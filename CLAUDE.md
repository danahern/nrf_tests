# Project: Embedded BLE Testing (nRF54L15)

## Serial Port Rules

**NEVER access serial ports directly** (e.g., `cat /dev/tty.*`, `screen`, `stty`, `picocom`). Serial ports are shared resources — opening them from here conflicts with other tools (minicom, serial monitors, etc.) and can lock the port.

Use `python3 zephyr_workspace/serial_monitor.py` to read serial output — it handles port access safely, resets the device, and captures 60s of logs. For just resetting, `nrfutil device reset --serial-number 1057709871` is fine.

**NOTE:** The serial monitor script may not work from Claude's sandbox — if it hangs, ask the user to run it from their terminal and share the output.

## Current Session Context

### iOS L2CAP Throughput Test App
- **App location**: `zephyr_workspace/L2CAPTest/` (open `.xcodeproj` in Xcode)
- **Status**: Built and running on iPhone 17 Pro Max
- **Source files**: `Sources/BLEManager.swift`, `Sources/ContentView.swift`, `Sources/L2CAPTestApp.swift`

### Test Results So Far
- **macOS baseline**: ~530 kbps (Python script, 15ms CI, ~4-5 packets/CE)
- **iOS test 1**: 446 kbps avg — worse than macOS
- **iOS test 2**: 83 kbps avg — suspiciously slow, likely DLE didn't negotiate or very long CI

### What We Were Doing
- Trying to get serial console logs from nRF to see what connection parameters iOS negotiated
- Need to see: `Conn params updated`, `Data Length updated`, `L2CAP channel connected` lines
- The 83 kbps suggests DLE may not have completed before channel opened, or iOS used a very long connection interval

### Next Steps
1. Run `python3 zephyr_workspace/serial_monitor.py` in user's terminal (not from Claude)
2. Tap Start in iOS app
3. Share serial output to analyze iOS connection parameters
4. Compare iOS CI/DLE/PHY vs macOS parameters
5. Log results in experiment table: `zephyr_workspace/nrf54l15_l2cap_test/PLAN.md`

### Key Files
- Firmware: `zephyr_workspace/nrf54l15_l2cap_test/src/main.c`
- Python test: `zephyr_workspace/l2cap_throughput_test.py`
- Serial monitor: `zephyr_workspace/serial_monitor.py`
- Experiment log: `zephyr_workspace/nrf54l15_l2cap_test/PLAN.md`
