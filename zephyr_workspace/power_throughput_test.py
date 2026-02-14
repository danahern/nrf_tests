#!/usr/bin/env python3
"""
Combined BLE throughput + PPK2 power measurement.

Runs GATT notification throughput test to macOS while simultaneously
measuring power consumption via Nordic PPK2.

Reports absolute timestamps, peak power, and per-second average power.

Usage:
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 power_throughput_test.py
"""

import asyncio
import time
import threading
import statistics
from bleak import BleakClient, BleakScanner
from ppk2_api.ppk2_api import PPK2_API

# Config
DEVICE_NAME = "nRF54L15_Test"
TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
PPK2_PORT = "/dev/tty.usbmodemC547BAC92FEA2"
PPK2_VOLTAGE_MV = 3000  # 3.0V (nRF54L15 VDDM range: 1.8-3.6V)
MEASURE_DURATION_S = 30
SETTLE_TIME_S = 5

# Shared state
rx_bytes = 0
start_time = None
measuring = False
ppk2_ready = threading.Event()

# Power data: list of (absolute_time, [samples_in_this_read])
power_log = []
power_lock = threading.Lock()

# Keep PPK2 object alive globally so it doesn't get GC'd
ppk2 = None


def notification_handler(sender, data):
    global rx_bytes
    rx_bytes += len(data)


def ppk2_measure_thread():
    """Collect power samples from PPK2 with timestamps (after BLE connects)."""
    global ppk2

    # Wait for BLE connection to stabilize
    while not measuring:
        time.sleep(0.1)

    time.sleep(SETTLE_TIME_S)
    print("PPK2: Starting power measurement...", flush=True)

    ppk2.start_measuring()
    measure_start = time.time()
    total_samples = 0

    while time.time() - measure_start < MEASURE_DURATION_S:
        read_data = ppk2.get_data()
        if read_data is not None:
            samples, raw_digital = ppk2.get_samples(read_data)
            ts = time.time()
            with power_lock:
                power_log.append((ts, samples))
            total_samples += len(samples)
        time.sleep(0.01)

    ppk2.stop_measuring()

    elapsed = time.time() - measure_start
    print(f"PPK2: Done. {total_samples:,} samples over {elapsed:.1f}s "
          f"({total_samples/elapsed:.0f} S/s)", flush=True)


async def run_test():
    global rx_bytes, start_time, measuring, ppk2

    # Step 1: Flush stale PPK2 serial data, then power up device
    print("PPK2: Flushing serial...", flush=True)
    import serial
    ser = serial.Serial(PPK2_PORT, 9600, timeout=0.1)
    ser.write(bytes([0x07]))  # PPK2 stop command
    import time as _time
    _time.sleep(0.5)
    while ser.in_waiting:
        ser.read(ser.in_waiting)
        _time.sleep(0.1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    ser.close()
    _time.sleep(2)

    print("PPK2: Initializing...", flush=True)
    ppk2 = PPK2_API(PPK2_PORT)
    ppk2.get_modifiers()
    ppk2.use_source_meter()
    ppk2.set_source_voltage(PPK2_VOLTAGE_MV)
    ppk2.toggle_DUT_power("ON")
    print(f"PPK2: Source mode at {PPK2_VOLTAGE_MV} mV, DUT power ON", flush=True)

    # Step 2: Wait for device to boot and start advertising
    print("Waiting 4s for device to boot...", flush=True)
    await asyncio.sleep(4)

    # Step 3: BLE scan
    print(f"BLE: Scanning for {DEVICE_NAME}...", flush=True)
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
    if device is None:
        print(f"ERROR: Could not find '{DEVICE_NAME}'")
        return

    print(f"BLE: Found {device.name}, connecting...", flush=True)

    # Step 4: Start measurement thread
    measure_t = threading.Thread(target=ppk2_measure_thread, daemon=True)
    measure_t.start()

    async with BleakClient(device) as client:
        print(f"BLE: Connected, MTU={client.mtu_size}", flush=True)
        await client.start_notify(TX_CHAR_UUID, notification_handler)
        print("BLE: Notifications enabled", flush=True)

        start_time = time.time()
        measuring = True

        total_time = SETTLE_TIME_S + MEASURE_DURATION_S + 2
        prev_bytes = 0
        for i in range(total_time):
            await asyncio.sleep(1.0)
            now = time.time()
            delta = rx_bytes - prev_bytes
            prev_bytes = rx_bytes
            instant_kbps = (delta * 8) / 1000
            avg_kbps = (rx_bytes * 8) / 1000 / (now - start_time)

            # Get latest power window
            with power_lock:
                if power_log:
                    latest = power_log[-1][1]
                    recent_avg_uA = statistics.mean(latest) if latest else 0
                    recent_peak_uA = max(latest) if latest else 0
                else:
                    recent_avg_uA = 0
                    recent_peak_uA = 0

            ts_str = time.strftime("%H:%M:%S", time.localtime(now))
            power_str = f"  power: avg={recent_avg_uA/1000:.2f} mA, peak={recent_peak_uA/1000:.2f} mA" if recent_avg_uA > 0 else ""
            print(f"  [{ts_str}] {instant_kbps:6.0f} kbps (inst) {avg_kbps:6.0f} kbps (avg){power_str}", flush=True)

    measure_t.join(timeout=5)

    # Aggregate all power samples
    all_samples = []
    with power_lock:
        for ts, samples in power_log:
            all_samples.extend([s for s in samples if 0 < s < 100000])

    elapsed = time.time() - start_time
    avg_throughput = (rx_bytes * 8) / 1000 / elapsed

    # Build per-second power profile
    print("\n" + "=" * 70, flush=True)
    print("PER-SECOND POWER PROFILE", flush=True)
    print("=" * 70, flush=True)
    print(f"{'Time':>10s}  {'Avg (mA)':>10s}  {'Peak (mA)':>10s}  {'Min (mA)':>10s}  {'Samples':>8s}", flush=True)
    print("-" * 70, flush=True)

    with power_lock:
        if power_log:
            first_ts = power_log[0][0]
            buckets = {}
            for ts, samples in power_log:
                sec = int(ts - first_ts)
                if sec not in buckets:
                    buckets[sec] = []
                buckets[sec].extend([s for s in samples if 0 < s < 100000])

            for sec in sorted(buckets.keys()):
                b = buckets[sec]
                if b:
                    abs_time = time.strftime("%H:%M:%S", time.localtime(first_ts + sec))
                    avg = statistics.mean(b) / 1000
                    peak = max(b) / 1000
                    mn = min(b) / 1000
                    print(f"{abs_time:>10s}  {avg:10.3f}  {peak:10.3f}  {mn:10.3f}  {len(b):8d}", flush=True)

    # Summary
    print("\n" + "=" * 70, flush=True)
    print("SUMMARY", flush=True)
    print("=" * 70, flush=True)

    print(f"\n--- Throughput ---", flush=True)
    print(f"Duration:       {elapsed:.1f} s", flush=True)
    print(f"Total RX:       {rx_bytes:,} bytes", flush=True)
    print(f"Avg throughput: {avg_throughput:.1f} kbps", flush=True)

    if all_samples:
        avg_uA = statistics.mean(all_samples)
        med_uA = statistics.median(all_samples)
        peak_uA = max(all_samples)
        min_uA = min(all_samples)
        std_uA = statistics.stdev(all_samples) if len(all_samples) > 1 else 0

        avg_mA = avg_uA / 1000
        peak_mA = peak_uA / 1000
        avg_mW = avg_mA * PPK2_VOLTAGE_MV / 1000
        peak_mW = peak_mA * PPK2_VOLTAGE_MV / 1000

        bits_per_sec = avg_throughput * 1000
        nJ_per_bit = (avg_mW * 1e6) / bits_per_sec if bits_per_sec > 0 else 0

        print(f"\n--- Power ({len(all_samples):,} samples) ---", flush=True)
        print(f"Average:    {avg_uA:.1f} uA  ({avg_mA:.3f} mA)", flush=True)
        print(f"Median:     {med_uA:.1f} uA  ({med_uA/1000:.3f} mA)", flush=True)
        print(f"Peak:       {peak_uA:.1f} uA  ({peak_mA:.3f} mA)", flush=True)
        print(f"Min:        {min_uA:.1f} uA  ({min_uA/1000:.3f} mA)", flush=True)
        print(f"Std dev:    {std_uA:.1f} uA", flush=True)
        print(f"Avg power:  {avg_mW:.3f} mW @ {PPK2_VOLTAGE_MV} mV", flush=True)
        print(f"Peak power: {peak_mW:.3f} mW @ {PPK2_VOLTAGE_MV} mV", flush=True)

        if nJ_per_bit > 0:
            print(f"\n--- Efficiency ---", flush=True)
            print(f"Energy/bit:  {nJ_per_bit:.1f} nJ/bit", flush=True)
            print(f"Energy/byte: {nJ_per_bit*8:.1f} nJ/byte", flush=True)
    else:
        print("\nNo valid power data collected", flush=True)

    print("=" * 70, flush=True)


if __name__ == "__main__":
    try:
        asyncio.run(run_test())
    except KeyboardInterrupt:
        print("\nStopped by user")
