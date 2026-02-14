#!/usr/bin/env python3
"""
Analyze power + throughput batch test results.

Reads power_throughput_raw.json and produces detailed statistics.

Usage:
    python3 power_analysis.py [power_throughput_raw.json]
"""

import json
import sys
import statistics
import time


def load_data(filename):
    with open(filename) as f:
        return json.load(f)


def analyze_run(run):
    """Analyze a single run, return summary dict."""
    power = run["power_per_second"]
    throughput = run["throughput_per_second"]

    if not power:
        return None

    # Power stats (from per-second averages)
    avg_currents = [p["avg_uA"] for p in power]
    peak_currents = [p["peak_uA"] for p in power]
    min_currents = [p["min_uA"] for p in power]
    median_currents = [p["median_uA"] for p in power]

    # Throughput stats (skip first 15s of ramp-up)
    steady_tp = [t for t in throughput if t["elapsed_s"] >= 15]
    if steady_tp:
        instant_rates = [t["instant_kbps"] for t in steady_tp]
    else:
        instant_rates = [t["instant_kbps"] for t in throughput]

    voltage_mV = run.get("ppk2_voltage_mV", 4000)

    overall_avg_uA = statistics.mean(avg_currents)
    overall_avg_mA = overall_avg_uA / 1000
    overall_peak_uA = max(peak_currents)
    overall_peak_mA = overall_peak_uA / 1000
    overall_min_uA = min(min_currents)
    avg_mW = overall_avg_mA * voltage_mV / 1000
    peak_mW = overall_peak_mA * voltage_mV / 1000

    avg_tp_kbps = statistics.mean(instant_rates)
    bits_per_sec = avg_tp_kbps * 1000
    nJ_per_bit = (avg_mW * 1e6) / bits_per_sec if bits_per_sec > 0 else 0

    return {
        "run": run["run_number"],
        "start_time": run["start_time_iso"],
        "duration_s": run["duration_s"],
        "throughput_kbps": round(avg_tp_kbps, 1),
        "throughput_std_kbps": round(statistics.stdev(instant_rates), 1) if len(instant_rates) > 1 else 0,
        "avg_current_uA": round(overall_avg_uA, 1),
        "avg_current_mA": round(overall_avg_mA, 3),
        "median_current_uA": round(statistics.median(avg_currents), 1),
        "peak_current_uA": round(overall_peak_uA, 1),
        "peak_current_mA": round(overall_peak_mA, 3),
        "min_current_uA": round(overall_min_uA, 1),
        "current_std_uA": round(statistics.stdev(avg_currents), 1) if len(avg_currents) > 1 else 0,
        "avg_power_mW": round(avg_mW, 3),
        "peak_power_mW": round(peak_mW, 3),
        "energy_per_bit_nJ": round(nJ_per_bit, 1),
        "energy_per_byte_nJ": round(nJ_per_bit * 8, 1),
        "total_bytes": run["total_bytes"],
        "power_samples_count": len(power),
    }


def print_per_run_table(summaries):
    """Print a table of per-run results."""
    print(f"\n{'Run':>4s}  {'Time':>10s}  {'Throughput':>12s}  {'Avg (mA)':>10s}  "
          f"{'Peak (mA)':>10s}  {'Min (uA)':>10s}  {'Power (mW)':>11s}  {'nJ/bit':>8s}")
    print("-" * 95)

    for s in summaries:
        t = s["start_time"].split("T")[1] if "T" in s["start_time"] else s["start_time"]
        print(f"{s['run']:4d}  {t:>10s}  {s['throughput_kbps']:9.1f} kbps"
              f"  {s['avg_current_mA']:10.3f}  {s['peak_current_mA']:10.3f}"
              f"  {s['min_current_uA']:10.1f}  {s['avg_power_mW']:11.3f}"
              f"  {s['energy_per_bit_nJ']:8.1f}")


def print_aggregate_stats(summaries):
    """Print aggregate statistics across all runs."""
    tp_vals = [s["throughput_kbps"] for s in summaries]
    avg_curr_vals = [s["avg_current_mA"] for s in summaries]
    peak_curr_vals = [s["peak_current_mA"] for s in summaries]
    power_vals = [s["avg_power_mW"] for s in summaries]
    eff_vals = [s["energy_per_bit_nJ"] for s in summaries]

    print(f"\n{'='*70}")
    print(f"AGGREGATE STATISTICS ({len(summaries)} runs)")
    print(f"{'='*70}")

    def stat_line(label, vals, unit, decimals=1):
        fmt = f"{{:.{decimals}f}}"
        avg = fmt.format(statistics.mean(vals))
        med = fmt.format(statistics.median(vals))
        mn = fmt.format(min(vals))
        mx = fmt.format(max(vals))
        std = fmt.format(statistics.stdev(vals)) if len(vals) > 1 else "N/A"
        print(f"  {label:<20s}  avg={avg:>10s}  med={med:>10s}  "
              f"min={mn:>10s}  max={mx:>10s}  std={std:>10s} {unit}")

    print(f"\n--- Throughput ---")
    stat_line("Rate", tp_vals, "kbps")

    print(f"\n--- Current ---")
    stat_line("Average", avg_curr_vals, "mA", 3)
    stat_line("Peak", peak_curr_vals, "mA", 3)

    print(f"\n--- Power ---")
    stat_line("Average", power_vals, "mW", 3)

    print(f"\n--- Efficiency ---")
    stat_line("Energy/bit", eff_vals, "nJ/bit")
    stat_line("Energy/byte", [v * 8 for v in eff_vals], "nJ/byte")

    # Total data transferred
    total_bytes = sum(s["total_bytes"] for s in summaries)
    total_duration = sum(s["duration_s"] for s in summaries)
    print(f"\n--- Totals ---")
    print(f"  Total data:     {total_bytes:,} bytes ({total_bytes/1024/1024:.1f} MB)")
    print(f"  Total duration: {total_duration:.0f}s ({total_duration/60:.1f} min)")


def print_time_series(data, run_idx=0):
    """Print per-second time series for a specific run."""
    run = data["runs"][run_idx]
    power = run["power_per_second"]
    throughput = run["throughput_per_second"]

    print(f"\n{'='*80}")
    print(f"TIME SERIES — Run {run['run_number']} ({run['start_time_iso']})")
    print(f"{'='*80}")
    print(f"{'Elapsed':>8s}  {'Time':>10s}  {'kbps':>8s}  {'Avg mA':>8s}  "
          f"{'Peak mA':>8s}  {'Min uA':>8s}  {'Samples':>8s}")
    print("-" * 80)

    # Build power lookup by elapsed second
    power_by_sec = {p["elapsed_s"]: p for p in power}

    for t in throughput:
        sec = int(t["elapsed_s"])
        ts = time.strftime("%H:%M:%S", time.localtime(t["timestamp"]))
        p = power_by_sec.get(sec, None)
        if p:
            print(f"{sec:7d}s  {ts:>10s}  {t['instant_kbps']:8.1f}"
                  f"  {p['avg_uA']/1000:8.3f}  {p['peak_uA']/1000:8.3f}"
                  f"  {p['min_uA']:8.1f}  {p['sample_count']:8d}")
        else:
            print(f"{sec:7d}s  {ts:>10s}  {t['instant_kbps']:8.1f}"
                  f"  {'---':>8s}  {'---':>8s}  {'---':>8s}  {'---':>8s}")


def main():
    filename = sys.argv[1] if len(sys.argv) > 1 else "power_throughput_raw.json"

    print(f"Loading {filename}...")
    data = load_data(filename)

    config = data["config"]
    runs = data["runs"]

    print(f"\n{'='*70}")
    print(f"POWER + THROUGHPUT ANALYSIS")
    print(f"{'='*70}")
    print(f"  Firmware:    {config.get('firmware', 'unknown')}")
    print(f"  Voltage:     {config['ppk2_voltage_mV']} mV")
    print(f"  Duration:    {config['measure_duration_s']}s per run")
    print(f"  Settle:      {config['settle_time_s']}s")
    print(f"  Runs:        {len(runs)}/{config['num_runs']}")
    print(f"  Date:        {config.get('test_date', 'unknown')}")

    # Analyze each run
    summaries = []
    for run in runs:
        s = analyze_run(run)
        if s:
            summaries.append(s)

    if not summaries:
        print("\nNo valid runs to analyze!")
        return

    # Per-run table
    print_per_run_table(summaries)

    # Aggregate stats
    print_aggregate_stats(summaries)

    # Time series for first and last run
    if len(runs) >= 1:
        print_time_series(data, 0)
    if len(runs) >= 2:
        print_time_series(data, len(runs) - 1)

    print(f"\n{'='*70}")


if __name__ == "__main__":
    main()
