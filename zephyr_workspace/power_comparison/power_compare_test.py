#!/usr/bin/env python3
"""
Power comparison test: nRF54LM20 vs Alif B1.

Measures current consumption via PPK2 in source meter mode across
idle, BLE advertising, GATT throughput, and L2CAP CoC throughput modes.

For throughput/l2cap modes, automatically launches the BLE central
receiver as a subprocess — no second terminal needed.

Usage:
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 power_compare_test.py \
        --platform nrf54lm20 \
        --modes idle advertising throughput l2cap \
        --runs 3
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time

from platforms import PLATFORMS, TEST_MODES
from ppk2_helper import init_ppk2, power_cycle, measure_power, cleanup_ppk2, find_ppk2_port
from flash_helper import flash_firmware

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PYTHON = sys.executable


def load_or_create_results(output_path, platform_config, modes, runs):
    """Load existing results or create new results dict."""
    try:
        with open(output_path) as f:
            results = json.load(f)
        print(f"Resuming from {output_path} ({len(results['measurements'])} existing measurements)", flush=True)
        return results
    except (FileNotFoundError, json.JSONDecodeError, KeyError):
        return {
            "config": {
                "platform": platform_config["name"],
                "ppk2_voltage_mV": platform_config["ppk2_voltage_mV"],
                "measurement_point": platform_config["measurement_point"],
                "test_date": time.strftime("%Y-%m-%d"),
                "modes": modes,
                "runs_per_mode": runs,
            },
            "measurements": [],
        }


def is_completed(results, mode, run):
    """Check if a specific mode/run combination is already done."""
    for m in results["measurements"]:
        if m["mode"] == mode and m["run_number"] == run:
            return True
    return False


def save_results(results, output_path):
    """Save results JSON (crash recovery: save after each measurement)."""
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w") as f:
        json.dump(results, f, indent=2)


def device_name_for_platform(platform_config):
    """Get the BLE device name for a platform."""
    name = platform_config["name"]
    # Match the CONFIG_BT_DEVICE_NAME / CONFIG_BLE_DEVICE_NAME in firmware
    if "nrf" in name.lower():
        return "nRF54LM20_Test"
    elif "alif" in name.lower():
        return "Alif_B1_Test"
    return name + "_Test"


def start_central(mode_name, device_name, duration):
    """Launch ble_central.py as a background subprocess.

    Returns the Popen object. Caller must stop it when done.
    """
    central_mode = "l2cap" if mode_name == "l2cap" else "gatt"
    central_script = os.path.join(SCRIPT_DIR, "ble_central.py")

    cmd = [PYTHON, central_script,
           "--mode", central_mode,
           "--name", device_name,
           "--duration", str(duration + 30)]  # extra time for connection setup

    print(f"  Starting BLE central ({central_mode} mode, device={device_name})...", flush=True)
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return proc


def wait_for_central_connection(proc, timeout=30):
    """Wait for the central to connect by watching its stdout."""
    start = time.time()
    while time.time() - start < timeout:
        if proc.poll() is not None:
            # Process exited — read remaining output
            out = proc.stdout.read()
            print(f"  Central exited early: {out}", flush=True)
            return False

        # Non-blocking read: check if there's output
        import select
        ready, _, _ = select.select([proc.stdout], [], [], 0.5)
        if ready:
            line = proc.stdout.readline()
            if line:
                line = line.strip()
                print(f"  [central] {line}", flush=True)
                # Look for connection/channel confirmation
                if any(kw in line.lower() for kw in ["connected", "channel opened", "receiving", "notifications"]):
                    # Give a moment for data flow to start
                    time.sleep(2)
                    return True

    print(f"  WARNING: Central didn't connect within {timeout}s", flush=True)
    return False


def stop_central(proc):
    """Stop the BLE central subprocess."""
    if proc and proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    # Drain any remaining output
    if proc and proc.stdout:
        remaining = proc.stdout.read()
        if remaining and remaining.strip():
            for line in remaining.strip().split("\n")[-3:]:
                print(f"  [central] {line}", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Power comparison test")
    parser.add_argument("--platform", required=True, choices=PLATFORMS.keys())
    parser.add_argument("--ppk2-port", help="PPK2 serial port (auto-detected if omitted)")
    parser.add_argument("--modes", nargs="+", default=["idle", "advertising", "throughput", "l2cap"],
                        choices=TEST_MODES.keys())
    parser.add_argument("--runs", type=int, default=3, help="Runs per mode")
    parser.add_argument("--output", help="Output JSON path (default: data/<platform>_power.json)")
    parser.add_argument("--serial-number", help="Device serial number (nRF: J-Link SNR)")
    parser.add_argument("--no-flash", action="store_true", help="Skip flashing (use current firmware)")
    args = parser.parse_args()

    platform = dict(PLATFORMS[args.platform])  # copy so we can modify
    if args.serial_number:
        platform["serial_number"] = args.serial_number
    output_path = args.output or os.path.join("data", f"{args.platform}_power.json")
    device_name = device_name_for_platform(platform)

    print(f"=== Power Comparison Test: {platform['name']} ===", flush=True)
    print(f"Voltage: {platform['ppk2_voltage_mV']} mV", flush=True)
    print(f"Modes: {', '.join(args.modes)}", flush=True)
    print(f"Runs per mode: {args.runs}", flush=True)
    print(f"BLE device name: {device_name}", flush=True)
    print(f"Output: {output_path}", flush=True)
    print(flush=True)

    ppk2_port = args.ppk2_port
    if not ppk2_port:
        ppk2_port = find_ppk2_port()
        if not ppk2_port:
            print("ERROR: No PPK2 found. Connect PPK2 or specify --ppk2-port.", flush=True)
            sys.exit(1)

    ppk2 = init_ppk2(ppk2_port, platform["ppk2_voltage_mV"])
    results = load_or_create_results(output_path, platform, args.modes, args.runs)

    last_flashed_mode = None

    for mode_name in args.modes:
        mode = TEST_MODES[mode_name]
        print(f"\n{'='*60}", flush=True)
        print(f"MODE: {mode['name']}", flush=True)
        print(f"{'='*60}", flush=True)

        for run in range(1, args.runs + 1):
            if is_completed(results, mode_name, run):
                print(f"  Run {run}/{args.runs}: already completed, skipping", flush=True)
                continue

            print(f"\n  Run {run}/{args.runs}", flush=True)

            central_proc = None

            # Flash if mode changed
            if not args.no_flash and last_flashed_mode != mode_name:
                ok = flash_firmware(platform, mode_name)
                if not ok:
                    print(f"  Flash FAILED, skipping mode {mode_name}", flush=True)
                    break
                last_flashed_mode = mode_name
            else:
                # Power cycle to reset device to clean state
                power_cycle(ppk2)

            # For throughput/l2cap, launch BLE central automatically
            if mode.get("requires_central"):
                central_proc = start_central(mode_name, device_name, mode["duration_s"])
                connected = wait_for_central_connection(central_proc, timeout=30)
                if not connected:
                    print(f"  Central failed to connect, skipping run", flush=True)
                    stop_central(central_proc)
                    continue

            # Measure
            power_data = measure_power(ppk2, mode["duration_s"], mode["settle_s"])

            # Stop central if running
            if central_proc:
                stop_central(central_proc)

            if not power_data:
                print(f"  WARNING: No power data collected", flush=True)
                continue

            # Compute summary stats
            avg_currents = [p["avg_uA"] for p in power_data]
            avg_uA = round(sum(avg_currents) / len(avg_currents), 1)
            avg_mA = round(avg_uA / 1000, 3)
            voltage_V = platform["ppk2_voltage_mV"] / 1000
            avg_mW = round(avg_mA * voltage_V, 3)
            peak_uA = round(max(p["peak_uA"] for p in power_data), 1)

            measurement = {
                "mode": mode_name,
                "mode_name": mode["name"],
                "run_number": run,
                "start_time_iso": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "duration_s": mode["duration_s"],
                "settle_s": mode["settle_s"],
                "ppk2_voltage_mV": platform["ppk2_voltage_mV"],
                "summary": {
                    "avg_uA": avg_uA,
                    "avg_mA": avg_mA,
                    "avg_mW": avg_mW,
                    "peak_uA": peak_uA,
                },
                "power_per_second": power_data,
            }

            results["measurements"].append(measurement)
            save_results(results, output_path)

            print(f"  Result: {avg_mA:.3f} mA avg, {peak_uA/1000:.3f} mA peak, "
                  f"{avg_mW:.3f} mW", flush=True)

    cleanup_ppk2(ppk2)

    print(f"\n{'='*60}", flush=True)
    print(f"DONE — {len(results['measurements'])} measurements saved to {output_path}", flush=True)
    print(f"Run: python3 power_compare_analysis.py {output_path} [other_platform.json]", flush=True)
    print(f"{'='*60}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped by user")
