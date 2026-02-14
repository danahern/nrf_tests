#!/usr/bin/env python3
"""
Batch BLE throughput + PPK2 power measurement.

Runs 10 consecutive 5-minute tests, saves all raw per-second data to JSON.
After completion, use power_analysis.py to analyze results.

Usage:
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 power_throughput_batch.py
"""

import asyncio
import time
import threading
import statistics
import json
from bleak import BleakClient, BleakScanner
from ppk2_api.ppk2_api import PPK2_API

# Config
DEVICE_NAME = "nRF54L15_Test"
TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
PPK2_PORT = "/dev/tty.usbmodemC547BAC92FEA2"
PPK2_VOLTAGE_MV = 1800
MEASURE_DURATION_S = 300  # 5 minutes
SETTLE_TIME_S = 10  # longer settle for cleaner data
NUM_RUNS = 5
OUTPUT_FILE = "power_throughput_1v8_no_usb.json"


def run_single_test(ppk2, run_number):
    """Run a single throughput+power test. Returns dict of results."""

    # Shared state for this run
    state = {
        "rx_bytes": 0,
        "start_time": None,
        "measuring": False,
        "power_log": [],  # list of (timestamp, [samples])
        "power_lock": threading.Lock(),
        "throughput_log": [],  # list of (timestamp, instant_kbps, avg_kbps)
    }

    def notification_handler(sender, data):
        state["rx_bytes"] += len(data)

    def ppk2_measure_thread():
        while not state["measuring"]:
            time.sleep(0.1)

        time.sleep(SETTLE_TIME_S)
        print(f"  PPK2: Starting measurement ({MEASURE_DURATION_S}s)...", flush=True)

        ppk2.start_measuring()
        measure_start = time.time()
        total_samples = 0

        while time.time() - measure_start < MEASURE_DURATION_S:
            read_data = ppk2.get_data()
            if read_data is not None:
                samples, _ = ppk2.get_samples(read_data)
                ts = time.time()
                with state["power_lock"]:
                    state["power_log"].append((ts, samples))
                total_samples += len(samples)
            time.sleep(0.01)

        ppk2.stop_measuring()
        elapsed = time.time() - measure_start
        print(f"  PPK2: {total_samples:,} samples over {elapsed:.1f}s", flush=True)

    async def ble_test():
        # Power cycle DUT
        print(f"  Power cycling DUT...", flush=True)
        ppk2.toggle_DUT_power("OFF")
        await asyncio.sleep(2)
        ppk2.toggle_DUT_power("ON")
        await asyncio.sleep(4)

        print(f"  BLE: Scanning...", flush=True)
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
        if device is None:
            print(f"  ERROR: Could not find '{DEVICE_NAME}'", flush=True)
            return False

        print(f"  BLE: Connecting...", flush=True)

        # Start measure thread
        measure_t = threading.Thread(target=ppk2_measure_thread, daemon=True)
        measure_t.start()

        async with BleakClient(device) as client:
            mtu = client.mtu_size
            print(f"  BLE: Connected, MTU={mtu}", flush=True)
            await client.start_notify(TX_CHAR_UUID, notification_handler)

            state["start_time"] = time.time()
            state["measuring"] = True

            total_time = SETTLE_TIME_S + MEASURE_DURATION_S + 2
            prev_bytes = 0
            for i in range(total_time):
                await asyncio.sleep(1.0)
                now = time.time()
                delta = state["rx_bytes"] - prev_bytes
                prev_bytes = state["rx_bytes"]
                instant_kbps = (delta * 8) / 1000
                elapsed = now - state["start_time"]
                avg_kbps = (state["rx_bytes"] * 8) / 1000 / elapsed

                state["throughput_log"].append({
                    "timestamp": now,
                    "elapsed_s": round(elapsed, 1),
                    "instant_kbps": round(instant_kbps, 1),
                    "avg_kbps": round(avg_kbps, 1),
                    "total_bytes": state["rx_bytes"],
                })

                # Print progress every 30s
                if i % 30 == 0:
                    with state["power_lock"]:
                        if state["power_log"]:
                            latest = state["power_log"][-1][1]
                            pwr = f", power: {statistics.mean(latest)/1000:.2f} mA" if latest else ""
                        else:
                            pwr = ""
                    print(f"  [{i:3d}s] {instant_kbps:.0f} kbps (inst) {avg_kbps:.0f} kbps (avg){pwr}", flush=True)

        measure_t.join(timeout=10)
        return True

    success = asyncio.run(ble_test())
    if not success:
        return None

    # Build per-second power summaries
    power_seconds = []
    with state["power_lock"]:
        if state["power_log"]:
            first_ts = state["power_log"][0][0]
            buckets = {}
            for ts, samples in state["power_log"]:
                sec = int(ts - first_ts)
                if sec not in buckets:
                    buckets[sec] = []
                buckets[sec].extend([s for s in samples if 0 < s < 200000])

            for sec in sorted(buckets.keys()):
                b = buckets[sec]
                if b:
                    power_seconds.append({
                        "timestamp": first_ts + sec,
                        "elapsed_s": sec,
                        "avg_uA": round(statistics.mean(b), 1),
                        "median_uA": round(statistics.median(b), 1),
                        "peak_uA": round(max(b), 1),
                        "min_uA": round(min(b), 1),
                        "std_uA": round(statistics.stdev(b), 1) if len(b) > 1 else 0,
                        "sample_count": len(b),
                    })

    elapsed = time.time() - state["start_time"]
    avg_throughput = (state["rx_bytes"] * 8) / 1000 / elapsed

    return {
        "run_number": run_number,
        "start_time_iso": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(state["start_time"])),
        "duration_s": round(elapsed, 1),
        "total_bytes": state["rx_bytes"],
        "avg_throughput_kbps": round(avg_throughput, 1),
        "ppk2_voltage_mV": PPK2_VOLTAGE_MV,
        "throughput_per_second": state["throughput_log"],
        "power_per_second": power_seconds,
    }


def main():
    print(f"=== Batch Power+Throughput Test ===", flush=True)
    print(f"Runs: {NUM_RUNS}, Duration: {MEASURE_DURATION_S}s each", flush=True)
    print(f"Output: {OUTPUT_FILE}", flush=True)
    print(flush=True)

    # Initialize PPK2 (stop any previous streaming, flush stale data)
    import serial
    ser = serial.Serial(PPK2_PORT, 9600, timeout=0.1)
    ser.write(bytes([0x07]))  # PPK2 stop command
    time.sleep(0.5)
    while ser.in_waiting:
        ser.read(ser.in_waiting)
        time.sleep(0.1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    ser.close()
    time.sleep(2)

    ppk2 = PPK2_API(PPK2_PORT)
    ppk2.get_modifiers()
    ppk2.use_source_meter()
    ppk2.set_source_voltage(PPK2_VOLTAGE_MV)
    ppk2.toggle_DUT_power("ON")
    print(f"PPK2: Source mode at {PPK2_VOLTAGE_MV} mV", flush=True)

    # Resume from existing data if available
    try:
        with open(OUTPUT_FILE) as f:
            all_results = json.load(f)
        existing_runs = len(all_results["runs"])
        print(f"Resuming: found {existing_runs} existing runs", flush=True)
    except (FileNotFoundError, json.JSONDecodeError):
        all_results = {
            "config": {
                "device_name": DEVICE_NAME,
                "ppk2_voltage_mV": PPK2_VOLTAGE_MV,
                "measure_duration_s": MEASURE_DURATION_S,
                "settle_time_s": SETTLE_TIME_S,
                "num_runs": NUM_RUNS,
                "firmware": "nrf54l15_ble_test (Zephyr LL, GATT notifications, 15ms CI, DCDC, no UART, no USB)",
                "test_date": time.strftime("%Y-%m-%d"),
            },
            "runs": [],
        }
        existing_runs = 0

    start_run = existing_runs + 1
    for run in range(start_run, NUM_RUNS + 1):
        print(f"\n{'='*60}", flush=True)
        print(f"RUN {run}/{NUM_RUNS}", flush=True)
        print(f"{'='*60}", flush=True)

        try:
            result = run_single_test(ppk2, run)
        except Exception as e:
            print(f"  Run {run} EXCEPTION: {e}", flush=True)
            result = None
        if result is None:
            print(f"  Run {run} FAILED, retrying in 10s...", flush=True)
            time.sleep(10)
            try:
                result = run_single_test(ppk2, run)
            except Exception as e:
                print(f"  Run {run} retry EXCEPTION: {e}", flush=True)
                result = None

        if result:
            all_results["runs"].append(result)
            # Quick summary
            power_data = result["power_per_second"]
            if power_data:
                avg_mA = statistics.mean([p["avg_uA"] for p in power_data]) / 1000
                peak_mA = max([p["peak_uA"] for p in power_data]) / 1000
                print(f"  Result: {result['avg_throughput_kbps']:.1f} kbps, "
                      f"{avg_mA:.2f} mA avg, {peak_mA:.2f} mA peak", flush=True)

            # Save after each run (in case of crash)
            with open(OUTPUT_FILE, "w") as f:
                json.dump(all_results, f, indent=2)
            print(f"  Saved to {OUTPUT_FILE}", flush=True)
        else:
            print(f"  Run {run} FAILED twice, skipping", flush=True)

    print(f"\n{'='*60}", flush=True)
    print(f"ALL DONE — {len(all_results['runs'])} runs completed", flush=True)
    print(f"Data saved to {OUTPUT_FILE}", flush=True)
    print(f"Run: python3 power_analysis.py to analyze", flush=True)
    print(f"{'='*60}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped by user")
