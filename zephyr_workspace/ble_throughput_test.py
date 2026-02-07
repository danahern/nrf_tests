#!/usr/bin/env python3
"""
BLE Throughput Test Script for nRF54L15
Continuously sends and receives data to measure throughput
Supports independent TX rate control for both Mac and nRF54L15 device
Can configure RISC-V workloads on dual-core app

Usage:
    python3 ble_throughput_test.py [--mac-tx KBPS] [--device-tx KBPS] [--workload 0-6] [--name NAME]

Examples:
    # Single-core app
    python3 ble_throughput_test.py                          # Max speed bidirectional (~185 kbps each)
    python3 ble_throughput_test.py --mac-tx 100             # Mac TX: 100 kbps, Device: max speed
    python3 ble_throughput_test.py --device-tx 50           # Mac TX: max, Device TX: 50 kbps
    python3 ble_throughput_test.py --mac-tx 0 --device-tx 50   # Mac RX only, Device: 50 kbps

    # Dual-core app
    python3 ble_throughput_test.py --name nRF54L15_Dual --workload 6   # Audio pipeline
    python3 ble_throughput_test.py --name nRF54L15_Dual --workload 5   # Mixed workload
    python3 ble_throughput_test.py --name nRF54L15_Dual --workload 0   # Idle (no workload)

    # Combined
    python3 ble_throughput_test.py --name nRF54L15_Dual --device-tx 48 --workload 6  # Audio + 48 kbps BLE
"""

import asyncio
import time
import argparse
from bleak import BleakClient, BleakScanner

# UUIDs for the throughput service
TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # Device TX (we receive)
RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Device RX (we send)
CTRL_CHAR_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  # Control (configure device TX rate)
RISCV_WORKLOAD_UUID = "6e400005-b5a3-f393-e0a9-e50e24dcca9e"  # RISC-V workload control
DEVICE_NAME = None  # Will be set by command line or default to "nRF54L15_Test"

# Target TX rate (kbps) - can be overridden by command line arg
TARGET_TX_KBPS = None  # None = max speed, 0 = RX only (no TX)
DEVICE_TX_KBPS = None  # None = max speed, 0 = disabled
RISCV_WORKLOAD = None  # None = don't change, 0-6 = set workload

# Stats tracking
rx_bytes = 0
tx_bytes = 0
start_time = None


def notification_handler(sender, data):
    """Handle notifications from device (data received)"""
    global rx_bytes
    rx_bytes += len(data)


async def print_stats():
    """Print throughput statistics every second"""
    global rx_bytes, tx_bytes, start_time

    while True:
        await asyncio.sleep(1.0)

        if start_time is None:
            continue

        elapsed = time.time() - start_time

        rx_kbps = (rx_bytes * 8) / 1000 / elapsed
        tx_kbps = (tx_bytes * 8) / 1000 / elapsed

        print(f"\n=== Throughput Stats (avg over {elapsed:.1f}s) ===")
        print(f"RX (from device): {rx_bytes:,} bytes ({rx_kbps:.1f} kbps)")
        print(f"TX (to device):   {tx_bytes:,} bytes ({tx_kbps:.1f} kbps)")
        print(f"Total:            {rx_bytes + tx_bytes:,} bytes")
        print("=" * 50)


async def send_data(client):
    """Continuously send data to device"""
    global tx_bytes, start_time

    # Check if TX is disabled
    if TARGET_TX_KBPS == 0:
        print("TX DISABLED - RX only mode (not sending data)")
        start_time = time.time()
        # Just wait forever
        while True:
            await asyncio.sleep(1)
        return

    # Create max size packet (495 bytes - matches device's 498 MTU, minus 3 byte ATT header)
    # Note: Bleak should handle MTU exchange automatically
    packet = bytes([i % 256 for i in range(495)])
    packet_size = len(packet)

    # Calculate delay based on target rate
    if TARGET_TX_KBPS is None:
        # Max speed - minimal delay
        delay = 0.01
        print("Starting continuous data transmission (MAX SPEED)...")
    else:
        # Calculate delay to achieve target rate
        # target_kbps = (bytes/sec * 8) / 1000
        # bytes/sec = (target_kbps * 1000) / 8
        # delay = packet_size / bytes_per_sec
        bytes_per_sec = (TARGET_TX_KBPS * 1000) / 8
        delay = packet_size / bytes_per_sec
        print(f"Starting continuous data transmission (TARGET: {TARGET_TX_KBPS} kbps)...")
        print(f"Sending {packet_size} byte packets every {delay*1000:.1f} ms")

    start_time = time.time()

    while True:
        try:
            await client.write_gatt_char(RX_CHAR_UUID, packet, response=False)
            tx_bytes += len(packet)

            # Delay based on target rate
            await asyncio.sleep(delay)

        except Exception as e:
            print(f"Error sending data: {e}")
            await asyncio.sleep(0.1)


async def main():
    """Main function to connect and run throughput test"""
    global rx_bytes, tx_bytes, start_time

    print(f"Scanning for {DEVICE_NAME}...")

    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)

    if device is None:
        print(f"ERROR: Could not find device '{DEVICE_NAME}'")
        print("Make sure the device is advertising and in range")
        return

    print(f"Found device: {device.name} ({device.address})")
    print("Connecting...")

    async with BleakClient(device) as client:
        print(f"Connected: {client.is_connected}")

        # Get MTU (Bleak negotiates this automatically on macOS)
        try:
            mtu = client.mtu_size
            print(f"Negotiated MTU: {mtu} bytes (max payload: {mtu-3} bytes)")
        except:
            print("MTU information not available")

        # Enable notifications on TX characteristic (receive data from device)
        print("Enabling notifications...")
        await client.start_notify(TX_CHAR_UUID, notification_handler)
        print("Notifications enabled")

        # Give connection time to stabilize
        await asyncio.sleep(1.5)

        # Configure device TX rate if specified
        if DEVICE_TX_KBPS is not None:
            print(f"Configuring device TX rate to {DEVICE_TX_KBPS} kbps...")
            # Send as little-endian uint32_t
            rate_bytes = DEVICE_TX_KBPS.to_bytes(4, byteorder='little')
            await client.write_gatt_char(CTRL_CHAR_UUID, rate_bytes, response=False)
            await asyncio.sleep(0.5)
        else:
            print("Device TX rate: MAX SPEED (default)")

        # Configure RISC-V workload if specified
        if RISCV_WORKLOAD is not None:
            workload_names = {
                0: "Idle",
                1: "Matrix Multiplication",
                2: "Sorting",
                3: "FFT Simulation",
                4: "Crypto Simulation",
                5: "Mixed",
                6: "Audio Pipeline (3 mics, beamforming, VAD)",
                7: "Audio Pipeline + AEC (3 mics, beamforming, VAD, echo cancellation)",
                8: "Proximity-Based VAD (near-field detection)",
                9: "Chest Resonance Detection (50-200 Hz)",
                10: "Clothing Rustle Suppression (impulse noise)",
                11: "Spatial Noise Cancellation (GSC + adaptive filter)",
                12: "Wind Noise Reduction (correlation-based)",
                13: "Full Necklace Pipeline (6-stage processing)"
            }
            workload_name = workload_names.get(RISCV_WORKLOAD, "Unknown")
            print(f"Configuring RISC-V workload to {RISCV_WORKLOAD} ({workload_name})...")
            # Send as single byte
            workload_byte = bytes([RISCV_WORKLOAD])
            await client.write_gatt_char(RISCV_WORKLOAD_UUID, workload_byte, response=False)
            await asyncio.sleep(0.5)
            print(f"RISC-V workload set to: {workload_name}")

        # Start stats display task
        stats_task = asyncio.create_task(print_stats())

        # Start sending data
        try:
            await send_data(client)
        except KeyboardInterrupt:
            print("\n\nTest stopped by user")
        finally:
            stats_task.cancel()

            # Print final stats
            if start_time:
                elapsed = time.time() - start_time
                print(f"\n=== Final Stats ===")
                print(f"Test duration: {elapsed:.1f} seconds")
                print(f"RX total: {rx_bytes:,} bytes ({(rx_bytes * 8 / 1000 / elapsed):.1f} kbps)")
                print(f"TX total: {tx_bytes:,} bytes ({(tx_bytes * 8 / 1000 / elapsed):.1f} kbps)")


if __name__ == "__main__":
    # Parse command line arguments
    parser = argparse.ArgumentParser(
        description='BLE Throughput Test for nRF54L15',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  %(prog)s                                    Max speed bidirectional (~185 kbps each)
  %(prog)s --mac-tx 100                       Mac TX: 100 kbps, Device: max speed
  %(prog)s --device-tx 50                     Mac TX: max, Device TX: 50 kbps
  %(prog)s --mac-tx 100 --device-tx 50        Mac: 100 kbps, Device: 50 kbps
  %(prog)s --mac-tx 0                         Mac doesn't send, Device: max speed
  %(prog)s --mac-tx 0 --device-tx 50          Mac RX only, Device: 50 kbps
  %(prog)s --name nRF54L15_Dual               Connect to dual-core app
        '''
    )

    parser.add_argument(
        '--mac-tx',
        type=int,
        metavar='KBPS',
        help='Mac to device TX rate in kbps (0 = disabled, omit = max speed)'
    )

    parser.add_argument(
        '--device-tx',
        type=int,
        metavar='KBPS',
        help='Device to Mac TX rate in kbps (0 = disabled, omit = max speed)'
    )

    parser.add_argument(
        '--name',
        type=str,
        metavar='NAME',
        default='nRF54L15_Test',
        help='Device name to scan for (default: nRF54L15_Test, dual-core: nRF54L15_Dual)'
    )

    parser.add_argument(
        '--workload',
        type=int,
        metavar='0-13',
        choices=range(0, 14),
        help='RISC-V workload (0=Idle, 1=Matrix, 2=Sort, 3=FFT, 4=Crypto, 5=Mixed, 6=Audio, 7=Audio+AEC, 8-13=Necklace algos)'
    )

    args = parser.parse_args()

    # Set device name
    DEVICE_NAME = args.name

    # Set Mac TX rate
    if args.mac_tx is not None:
        if args.mac_tx < 0:
            print("Error: Mac TX rate must be 0 or positive")
            exit(1)
        TARGET_TX_KBPS = args.mac_tx
        if TARGET_TX_KBPS == 0:
            print("Mac TX: DISABLED (RX only mode)")
        else:
            print(f"Mac TX rate: {TARGET_TX_KBPS} kbps")
    else:
        print("Mac TX rate: MAX SPEED")

    # Set device TX rate
    if args.device_tx is not None:
        if args.device_tx < 0:
            print("Error: Device TX rate must be 0 or positive")
            exit(1)
        DEVICE_TX_KBPS = args.device_tx
        print(f"Device TX rate: {DEVICE_TX_KBPS} kbps")
    else:
        print("Device TX rate: MAX SPEED (default)")

    # Set RISC-V workload
    if args.workload is not None:
        RISCV_WORKLOAD = args.workload
        workload_names = {
            0: "Idle",
            1: "Matrix Multiplication",
            2: "Sorting",
            3: "FFT Simulation",
            4: "Crypto Simulation",
            5: "Mixed",
            6: "Audio Pipeline (3 mics, beamforming, VAD)",
            7: "Audio Pipeline + AEC (3 mics, beamforming, VAD, echo cancellation)",
            8: "Proximity-Based VAD (near-field detection)",
            9: "Chest Resonance Detection (50-200 Hz)",
            10: "Clothing Rustle Suppression (impulse noise)",
            11: "Spatial Noise Cancellation (GSC + adaptive filter)",
            12: "Wind Noise Reduction (correlation-based)",
            13: "Full Necklace Pipeline (6-stage processing)"
        }
        print(f"RISC-V workload: {RISCV_WORKLOAD} ({workload_names[RISCV_WORKLOAD]})")
    else:
        print("RISC-V workload: Not configured (will use device default)")

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nExiting...")
