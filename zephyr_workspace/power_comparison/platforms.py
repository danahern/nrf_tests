"""
Platform and test mode definitions for power comparison.

Update firmware paths after building each project.
"""

import os

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SETOOLS_DIR = os.path.join(os.path.dirname(BASE_DIR), "alif_security_tools", "app-release-exec-macos")
ALIF_SDK_DIR = os.path.join(os.path.dirname(BASE_DIR), "sdk-alif")

PLATFORMS = {
    "nrf54lm20": {
        "name": "nRF54LM20",
        "ppk2_voltage_mV": 1800,
        "flash_method": "nrfjprog",
        "serial_number": None,  # Set via CLI --serial-number
        "measurement_point": "P14 (VDD nRF, SB10 cut)",
        "firmware": {
            "idle": os.path.join(BASE_DIR, "nrf54lm20_idle_test", "build", "zephyr", "zephyr.hex"),
            "advertising": os.path.join(BASE_DIR, "nrf54lm20_adv_test", "build", "zephyr", "zephyr.hex"),
            "throughput": os.path.join(BASE_DIR, "nrf54lm20_throughput_test", "build", "zephyr", "zephyr.hex"),
            "l2cap": os.path.join(BASE_DIR, "nrf54lm20_l2cap_test", "build", "zephyr", "zephyr.hex"),
        },
    },
    "alif_b1": {
        "name": "Alif B1",
        "ppk2_voltage_mV": 3300,
        "flash_method": "alif_setools",
        "serial_number": None,
        "measurement_point": "JP4 (VDD_MAIN, trace cut)",
        "setools_dir": SETOOLS_DIR,
        "firmware": {
            "idle": os.path.join(BASE_DIR, "alif_b1_idle_test", "build", "zephyr", "zephyr.bin"),
            "advertising": os.path.join(BASE_DIR, "alif_b1_adv_test", "build", "zephyr", "zephyr.bin"),
            "throughput": os.path.join(BASE_DIR, "alif_b1_throughput_test", "build", "zephyr", "zephyr.bin"),
            "l2cap": os.path.join(BASE_DIR, "alif_b1_l2cap_test", "build", "zephyr", "zephyr.bin"),
        },
    },
}

TEST_MODES = {
    "idle": {
        "name": "Deep Sleep / Lowest Power Idle",
        "duration_s": 60,
        "settle_s": 10,
        "description": "Device in deepest sleep with periodic 1s wakeup",
    },
    "advertising": {
        "name": "BLE Advertising",
        "duration_s": 60,
        "settle_s": 10,
        "description": "Non-connectable BLE advertising at 1s interval",
    },
    "throughput": {
        "name": "BLE Throughput (GATT Notifications)",
        "duration_s": 120,
        "settle_s": 15,
        "description": "Active BLE GATT notification streaming (244B payloads)",
        "requires_central": True,
    },
    "l2cap": {
        "name": "BLE Throughput (L2CAP CoC)",
        "duration_s": 120,
        "settle_s": 15,
        "description": "Active BLE L2CAP CoC streaming (492B SDUs)",
        "requires_central": True,
    },
}
