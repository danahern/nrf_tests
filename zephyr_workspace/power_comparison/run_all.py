#!/usr/bin/env python3
"""
Run full power comparison: nRF54LM20 vs Alif B1.

Single command that runs all 4 modes on both platforms, then generates
the comparison report. Automatically detects PPK2 and launches BLE
central for throughput modes.

Usage:
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 run_all.py
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 run_all.py --platforms nrf54lm20
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 run_all.py --modes idle advertising
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 run_all.py --runs 5
"""

import argparse
import os
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PYTHON = sys.executable


def run_platform_test(platform, modes, runs, ppk2_port, serial_number, output):
    """Run power_compare_test.py for one platform."""
    cmd = [PYTHON, os.path.join(SCRIPT_DIR, "power_compare_test.py"),
           "--platform", platform,
           "--modes"] + modes + [
           "--runs", str(runs)]

    if ppk2_port:
        cmd.extend(["--ppk2-port", ppk2_port])
    if serial_number:
        cmd.extend(["--serial-number", serial_number])
    if output:
        cmd.extend(["--output", output])

    import subprocess
    print(f"\n{'#'*70}", flush=True)
    print(f"# Testing: {platform}", flush=True)
    print(f"# Modes: {', '.join(modes)}", flush=True)
    print(f"# Runs per mode: {runs}", flush=True)
    print(f"{'#'*70}\n", flush=True)

    result = subprocess.run(cmd, cwd=SCRIPT_DIR)
    return result.returncode == 0


def run_analysis(nrf_json, alif_json):
    """Run power_compare_analysis.py on the results."""
    import subprocess
    cmd = [PYTHON, os.path.join(SCRIPT_DIR, "power_compare_analysis.py")]

    files = []
    if os.path.exists(nrf_json):
        files.append(nrf_json)
    if os.path.exists(alif_json):
        files.append(alif_json)

    if not files:
        print("No result files found — nothing to analyze.", flush=True)
        return

    cmd.extend(files)
    print(f"\n{'#'*70}", flush=True)
    print(f"# Generating comparison report", flush=True)
    print(f"{'#'*70}\n", flush=True)
    subprocess.run(cmd, cwd=SCRIPT_DIR)


def main():
    parser = argparse.ArgumentParser(
        description="Run full power comparison: nRF54LM20 vs Alif B1")
    parser.add_argument("--platforms", nargs="+",
                        default=["nrf54lm20", "alif_b1"],
                        choices=["nrf54lm20", "alif_b1"],
                        help="Platforms to test (default: both)")
    parser.add_argument("--modes", nargs="+",
                        default=["idle", "advertising", "throughput", "l2cap"],
                        choices=["idle", "advertising", "throughput", "l2cap"],
                        help="Test modes (default: all 4)")
    parser.add_argument("--runs", type=int, default=3,
                        help="Measurement runs per mode (default: 3)")
    parser.add_argument("--ppk2-port",
                        help="PPK2 serial port (auto-detected if omitted)")
    parser.add_argument("--nrf-serial",
                        help="nRF J-Link serial number")
    args = parser.parse_args()

    nrf_json = os.path.join(SCRIPT_DIR, "data", "nrf54lm20_power.json")
    alif_json = os.path.join(SCRIPT_DIR, "data", "alif_b1_power.json")

    start_time = time.time()

    print("=" * 70, flush=True)
    print("POWER COMPARISON: nRF54LM20 vs Alif B1", flush=True)
    print(f"Platforms: {', '.join(args.platforms)}", flush=True)
    print(f"Modes: {', '.join(args.modes)}", flush=True)
    print(f"Runs: {args.runs} per mode", flush=True)
    print(f"Total measurements: {len(args.platforms) * len(args.modes) * args.runs}", flush=True)
    print("=" * 70, flush=True)

    # Note: between platforms, user needs to swap PPK2 wiring
    for i, platform in enumerate(args.platforms):
        if i > 0:
            print(f"\n{'!'*70}", flush=True)
            print(f"! Swap PPK2 to {platform} board now.", flush=True)
            if platform == "alif_b1":
                print(f"! PPK2 VOUT -> JP4 (VDD_MAIN), voltage will be set to 3300 mV", flush=True)
            else:
                print(f"! PPK2 VOUT -> P14 (VDD nRF), voltage will be set to 1800 mV", flush=True)
            print(f"{'!'*70}", flush=True)
            input("Press Enter when PPK2 is connected to the new board...")

        serial_number = args.nrf_serial if platform == "nrf54lm20" else None
        output = nrf_json if platform == "nrf54lm20" else alif_json

        ok = run_platform_test(
            platform=platform,
            modes=args.modes,
            runs=args.runs,
            ppk2_port=args.ppk2_port,
            serial_number=serial_number,
            output=output,
        )
        if not ok:
            print(f"\nWARNING: {platform} test had errors", flush=True)

    # Generate comparison report
    run_analysis(nrf_json, alif_json)

    elapsed = time.time() - start_time
    print(f"\n{'='*70}", flush=True)
    print(f"ALL DONE in {elapsed/60:.1f} minutes", flush=True)
    print(f"Results: {nrf_json}", flush=True)
    print(f"         {alif_json}", flush=True)
    print(f"Report:  {os.path.join(SCRIPT_DIR, 'data', 'COMPARISON_REPORT.md')}", flush=True)
    print(f"{'='*70}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped by user")
