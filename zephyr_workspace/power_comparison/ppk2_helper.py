"""
PPK2 helper functions for power measurement.

Extracted from power_throughput_batch.py and power_throughput_test.py.
Handles PPK2 initialization, power cycling, and current measurement collection.
"""

import glob
import time
import statistics
import serial
from ppk2_api.ppk2_api import PPK2_API


def find_ppk2_port():
    """Auto-detect PPK2 serial port on macOS.

    Scans /dev/tty.usbmodem* and tries to identify the PPK2 by sending
    a stop command and checking for a valid response.
    Returns the port path, or None if not found.
    """
    candidates = sorted(glob.glob("/dev/tty.usbmodem*"))
    if not candidates:
        return None

    for port in candidates:
        try:
            ser = serial.Serial(port, 9600, timeout=0.5)
            ser.write(bytes([0x07]))  # PPK2 stop command
            time.sleep(0.3)
            # PPK2 responds to commands — if we can open it and it doesn't throw, it's likely a PPK2
            ser.close()
            # Try to init PPK2 API — this is the definitive check
            ppk2 = PPK2_API(port)
            ppk2.get_modifiers()
            # If we get here, it's a real PPK2
            del ppk2
            time.sleep(0.5)
            print(f"PPK2: Auto-detected at {port}", flush=True)
            return port
        except Exception:
            continue

    return None


def init_ppk2(port, voltage_mV):
    """Initialize PPK2 in source meter mode.

    Flushes stale serial data, sends stop command, then initializes
    the PPK2 API in source meter mode at the specified voltage.

    Returns the PPK2_API instance with DUT power ON.
    """
    # Flush stale data (pattern from power_throughput_batch.py:184-194)
    ser = serial.Serial(port, 9600, timeout=0.1)
    ser.write(bytes([0x07]))  # PPK2 stop command
    time.sleep(0.5)
    while ser.in_waiting:
        ser.read(ser.in_waiting)
        time.sleep(0.1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    ser.close()
    time.sleep(2)

    ppk2 = PPK2_API(port)
    ppk2.get_modifiers()
    ppk2.use_source_meter()
    ppk2.set_source_voltage(voltage_mV)
    ppk2.toggle_DUT_power("ON")
    print(f"PPK2: Source mode at {voltage_mV} mV, DUT power ON", flush=True)
    return ppk2


def power_cycle(ppk2, off_s=2, on_s=4):
    """Power cycle the DUT via PPK2."""
    print("  Power cycling DUT...", flush=True)
    ppk2.toggle_DUT_power("OFF")
    time.sleep(off_s)
    ppk2.toggle_DUT_power("ON")
    time.sleep(on_s)


def measure_power(ppk2, duration_s, settle_s=10):
    """Collect per-second current samples for the given duration.

    Waits settle_s seconds before starting measurement.
    Returns a list of per-second dicts with keys:
        elapsed_s, avg_uA, median_uA, peak_uA, min_uA, std_uA, sample_count
    """
    print(f"  Settling {settle_s}s...", flush=True)
    time.sleep(settle_s)

    print(f"  Measuring {duration_s}s...", flush=True)
    ppk2.start_measuring()
    measure_start = time.time()
    raw_log = []  # list of (timestamp, [samples])
    total_samples = 0

    while time.time() - measure_start < duration_s:
        read_data = ppk2.get_data()
        if read_data is not None:
            samples, _ = ppk2.get_samples(read_data)
            raw_log.append((time.time(), samples))
            total_samples += len(samples)
        time.sleep(0.01)

    ppk2.stop_measuring()
    elapsed = time.time() - measure_start
    print(f"  {total_samples:,} samples over {elapsed:.1f}s", flush=True)

    # Build per-second summaries (pattern from power_throughput_batch.py:136-160)
    if not raw_log:
        return []

    first_ts = raw_log[0][0]
    buckets = {}
    for ts, samples in raw_log:
        sec = int(ts - first_ts)
        if sec not in buckets:
            buckets[sec] = []
        buckets[sec].extend(s for s in samples if 0 < s < 200000)

    power_seconds = []
    for sec in sorted(buckets.keys()):
        b = buckets[sec]
        if b:
            power_seconds.append({
                "elapsed_s": sec,
                "avg_uA": round(statistics.mean(b), 1),
                "median_uA": round(statistics.median(b), 1),
                "peak_uA": round(max(b), 1),
                "min_uA": round(min(b), 1),
                "std_uA": round(statistics.stdev(b), 1) if len(b) > 1 else 0,
                "sample_count": len(b),
            })

    return power_seconds


def cleanup_ppk2(ppk2):
    """Stop measurement and power off DUT."""
    try:
        ppk2.stop_measuring()
    except Exception:
        pass
    ppk2.toggle_DUT_power("OFF")
    print("PPK2: DUT power OFF", flush=True)
