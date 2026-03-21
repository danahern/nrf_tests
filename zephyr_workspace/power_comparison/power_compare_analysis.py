#!/usr/bin/env python3
"""
Generate power comparison report from platform test result files.

Usage:
    python3 power_compare_analysis.py data/nrf54lm20_power.json data/alif_b1_power.json
    python3 power_compare_analysis.py data/nrf54lm20_power.json  # single platform
"""

import json
import sys
import statistics
import os


def load_data(filename):
    with open(filename) as f:
        return json.load(f)


def analyze_mode(measurements, mode_name):
    """Analyze all runs for a given mode. Returns summary dict."""
    mode_runs = [m for m in measurements if m["mode"] == mode_name]
    if not mode_runs:
        return None

    voltage_mV = mode_runs[0]["ppk2_voltage_mV"]
    voltage_V = voltage_mV / 1000

    avg_currents = [m["summary"]["avg_uA"] for m in mode_runs]
    peak_currents = [m["summary"]["peak_uA"] for m in mode_runs]
    avg_powers = [m["summary"]["avg_mW"] for m in mode_runs]

    return {
        "mode": mode_name,
        "mode_name": mode_runs[0]["mode_name"],
        "num_runs": len(mode_runs),
        "voltage_mV": voltage_mV,
        "avg_current_uA": round(statistics.mean(avg_currents), 1),
        "avg_current_mA": round(statistics.mean(avg_currents) / 1000, 3),
        "std_current_uA": round(statistics.stdev(avg_currents), 1) if len(avg_currents) > 1 else 0,
        "peak_current_uA": round(max(peak_currents), 1),
        "avg_power_mW": round(statistics.mean(avg_powers), 3),
        "battery_life_hours_1Wh": round(1000 / statistics.mean(avg_powers), 1) if statistics.mean(avg_powers) > 0 else float("inf"),
    }


def print_single_platform(data):
    """Print analysis for a single platform."""
    config = data["config"]
    measurements = data["measurements"]

    print(f"\n{'='*70}")
    print(f"POWER ANALYSIS: {config['platform']}")
    print(f"{'='*70}")
    print(f"  Voltage:     {config['ppk2_voltage_mV']} mV")
    print(f"  Measurement: {config.get('measurement_point', 'N/A')}")
    print(f"  Date:        {config.get('test_date', 'N/A')}")
    print(f"  Runs/mode:   {config.get('runs_per_mode', 'N/A')}")

    modes = sorted(set(m["mode"] for m in measurements))
    summaries = []
    for mode in modes:
        s = analyze_mode(measurements, mode)
        if s:
            summaries.append(s)

    print(f"\n{'Mode':<30s}  {'Avg (mA)':>10s}  {'Peak (mA)':>10s}  "
          f"{'Power (mW)':>11s}  {'Battery (h)':>12s}")
    print("-" * 85)
    for s in summaries:
        print(f"{s['mode_name']:<30s}  {s['avg_current_mA']:10.3f}  "
              f"{s['peak_current_uA']/1000:10.3f}  {s['avg_power_mW']:11.3f}  "
              f"{s['battery_life_hours_1Wh']:12.1f}")

    return summaries


def print_comparison(data_a, data_b):
    """Print side-by-side comparison of two platforms."""
    config_a = data_a["config"]
    config_b = data_b["config"]
    name_a = config_a["platform"]
    name_b = config_b["platform"]

    print(f"\n{'='*90}")
    print(f"POWER COMPARISON: {name_a} vs {name_b}")
    print(f"{'='*90}")
    print(f"  {name_a}: {config_a['ppk2_voltage_mV']} mV, {config_a.get('measurement_point', 'N/A')}")
    print(f"  {name_b}: {config_b['ppk2_voltage_mV']} mV, {config_b.get('measurement_point', 'N/A')}")
    print(f"\n  NOTE: Different voltages — compare power (mW), not current (mA)")

    modes_a = sorted(set(m["mode"] for m in data_a["measurements"]))
    modes_b = sorted(set(m["mode"] for m in data_b["measurements"]))
    all_modes = sorted(set(modes_a + modes_b))

    # Header
    w = 12
    print(f"\n{'Mode':<25s}  "
          f"{'':>{w}s} {name_a:>{w}s} {'':>{w}s}  "
          f"{'':>{w}s} {name_b:>{w}s} {'':>{w}s}  "
          f"{'Winner':>{w}s}")
    print(f"{'':25s}  "
          f"{'Avg mA':>{w}s} {'Power mW':>{w}s} {'Batt hrs':>{w}s}  "
          f"{'Avg mA':>{w}s} {'Power mW':>{w}s} {'Batt hrs':>{w}s}  "
          f"{'(lower mW)':>{w}s}")
    print("-" * 130)

    for mode in all_modes:
        sa = analyze_mode(data_a["measurements"], mode)
        sb = analyze_mode(data_b["measurements"], mode)

        mode_label = (sa or sb)["mode_name"] if (sa or sb) else mode

        def fmt(s):
            if s is None:
                return f"{'N/A':>{w}s}", f"{'N/A':>{w}s}", f"{'N/A':>{w}s}"
            return (f"{s['avg_current_mA']:>{w}.3f}",
                    f"{s['avg_power_mW']:>{w}.3f}",
                    f"{s['battery_life_hours_1Wh']:>{w}.1f}")

        a_mA, a_mW, a_batt = fmt(sa)
        b_mA, b_mW, b_batt = fmt(sb)

        if sa and sb:
            winner = name_a if sa["avg_power_mW"] < sb["avg_power_mW"] else name_b
            pct = abs(sa["avg_power_mW"] - sb["avg_power_mW"]) / max(sa["avg_power_mW"], sb["avg_power_mW"]) * 100
            winner_str = f"{winner} ({pct:.0f}%)"
        else:
            winner_str = "N/A"

        print(f"{mode_label:<25s}  "
              f"{a_mA} {a_mW} {a_batt}  "
              f"{b_mA} {b_mW} {b_batt}  "
              f"{winner_str:>{w}s}")

    print(f"\n{'='*90}")


def generate_markdown_report(data_a, data_b=None, output_path=None):
    """Generate a markdown comparison report."""
    lines = ["# Power Comparison Report\n"]
    lines.append(f"Generated: {__import__('time').strftime('%Y-%m-%d %H:%M:%S')}\n")

    platforms = [data_a]
    if data_b:
        platforms.append(data_b)

    lines.append("## Test Configuration\n")
    lines.append("| Parameter | " + " | ".join(d["config"]["platform"] for d in platforms) + " |")
    lines.append("| --- | " + " | ".join("---" for _ in platforms) + " |")
    lines.append("| Voltage | " + " | ".join(f"{d['config']['ppk2_voltage_mV']} mV" for d in platforms) + " |")
    lines.append("| Measurement Point | " + " | ".join(d["config"].get("measurement_point", "N/A") for d in platforms) + " |")
    lines.append("| Date | " + " | ".join(d["config"].get("test_date", "N/A") for d in platforms) + " |")
    lines.append("")

    if data_b:
        lines.append("> **Note:** Platforms run at different voltages. Compare **power (mW)** and **energy efficiency (nJ/bit)**, not raw current.\n")

    all_modes = sorted(set(
        m["mode"] for d in platforms for m in d["measurements"]
    ))

    lines.append("## Results\n")
    header = "| Mode | " + " | ".join(
        f"{d['config']['platform']} avg mA | {d['config']['platform']} mW | {d['config']['platform']} battery (1Wh)" for d in platforms
    ) + " |"
    sep = "| --- | " + " | ".join("---: | ---: | ---:" for _ in platforms) + " |"
    lines.append(header)
    lines.append(sep)

    for mode in all_modes:
        row = [mode]
        for d in platforms:
            s = analyze_mode(d["measurements"], mode)
            if s:
                row.extend([f"{s['avg_current_mA']:.3f}", f"{s['avg_power_mW']:.3f}", f"{s['battery_life_hours_1Wh']:.1f}h"])
            else:
                row.extend(["N/A", "N/A", "N/A"])
        lines.append("| " + " | ".join(row) + " |")

    lines.append("")

    report = "\n".join(lines)
    if output_path:
        with open(output_path, "w") as f:
            f.write(report)
        print(f"\nMarkdown report saved to {output_path}")
    return report


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 power_compare_analysis.py <platform1.json> [platform2.json]")
        sys.exit(1)

    data_a = load_data(sys.argv[1])
    data_b = load_data(sys.argv[2]) if len(sys.argv) > 2 else None

    print_single_platform(data_a)
    if data_b:
        print_single_platform(data_b)
        print_comparison(data_a, data_b)

    # Generate markdown report
    report_path = os.path.join("data", "COMPARISON_REPORT.md")
    generate_markdown_report(data_a, data_b, report_path)


if __name__ == "__main__":
    main()
