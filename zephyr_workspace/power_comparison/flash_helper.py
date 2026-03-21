"""
Flash helper for nRF54LM20 and Alif B1 platforms.

Dispatches to the correct flash tool based on platform configuration.
Assumes PPK2 is already powering the device.
"""

import subprocess
import json
import os
import shutil


def flash_nrf(hex_path, serial_number=None):
    """Flash nRF54LM20 via nrfjprog.

    Args:
        hex_path: Path to the .hex firmware file.
        serial_number: J-Link serial number (optional).

    Returns True on success.
    """
    if not os.path.exists(hex_path):
        print(f"  ERROR: Firmware not found: {hex_path}", flush=True)
        return False

    cmd = ["nrfjprog", "--program", hex_path, "--verify", "--reset"]
    if serial_number:
        cmd.extend(["--snr", str(serial_number)])

    print(f"  Flashing nRF: {os.path.basename(hex_path)}", flush=True)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  Flash FAILED: {result.stderr.strip()}", flush=True)
        return False

    print(f"  Flash OK", flush=True)
    return True


def flash_alif(bin_path, setools_dir, cpu_id="M55_HE", load_address="0x58000000"):
    """Flash Alif B1 via app-gen-toc + app-write-mram.

    Args:
        bin_path: Path to the .bin firmware file.
        setools_dir: Path to alif_security_tools/app-release-exec-macos/.
        cpu_id: Target CPU (default M55_HE).
        load_address: MRAM load address.

    Returns True on success.
    """
    if not os.path.exists(bin_path):
        print(f"  ERROR: Firmware not found: {bin_path}", flush=True)
        return False

    build_dir = os.path.join(setools_dir, "build")
    images_dir = os.path.join(build_dir, "images")
    config_dir = os.path.join(build_dir, "config")

    # Copy firmware to images dir
    dest = os.path.join(images_dir, os.path.basename(bin_path))
    shutil.copy2(bin_path, dest)

    # Write app-cfg.json
    app_cfg = {
        "DEVICE": {
            "disabled": False,
            "binary": "app-device-config.json",
            "version": "0.5.00",
            "signed": True,
        },
        "APP": {
            "disabled": False,
            "binary": os.path.basename(bin_path),
            "version": "1.0.0",
            "signed": True,
            "cpu_id": cpu_id,
            "loadAddress": load_address,
            "flags": ["load", "boot"],
        },
    }
    cfg_path = os.path.join(config_dir, "app-cfg.json")
    with open(cfg_path, "w") as f:
        json.dump(app_cfg, f, indent=4)

    # Run app-gen-toc
    print(f"  Alif: Generating TOC for {os.path.basename(bin_path)}...", flush=True)
    gen_toc = os.path.join(setools_dir, "app-gen-toc")
    result = subprocess.run([gen_toc], cwd=setools_dir, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  app-gen-toc FAILED: {result.stderr.strip()}", flush=True)
        return False

    # Run app-write-mram
    print(f"  Alif: Writing to MRAM...", flush=True)
    write_mram = os.path.join(setools_dir, "app-write-mram")
    result = subprocess.run([write_mram], cwd=setools_dir, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  app-write-mram FAILED: {result.stderr.strip()}", flush=True)
        return False

    print(f"  Flash OK", flush=True)
    return True


def flash_firmware(platform_config, mode):
    """Flash firmware for the given platform and test mode.

    Args:
        platform_config: Platform dict from platforms.py.
        mode: Test mode name ("idle", "advertising", "throughput").

    Returns True on success.
    """
    fw_path = platform_config["firmware"].get(mode)
    if not fw_path:
        print(f"  ERROR: No firmware configured for {platform_config['name']} / {mode}", flush=True)
        return False

    method = platform_config["flash_method"]
    if method == "nrfjprog":
        return flash_nrf(fw_path, platform_config.get("serial_number"))
    elif method == "alif_setools":
        return flash_alif(fw_path, platform_config["setools_dir"])
    else:
        print(f"  ERROR: Unknown flash method: {method}", flush=True)
        return False
