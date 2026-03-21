#!/usr/bin/env python3
"""
BLE Central receiver for power comparison throughput tests.

Supports both GATT notifications (via bleak) and L2CAP CoC (via CoreBluetooth).
Use with the power_compare_test.py measurement script.

Usage:
    # GATT notifications (works with nrf54lm20_throughput_test and alif_b1_throughput_test)
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 ble_central.py --mode gatt --name nRF54LM20_Test

    # L2CAP CoC (works with nrf54lm20_l2cap_test and alif_b1_l2cap_test)
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 ble_central.py --mode l2cap --name nRF54LM20_Test

    # Auto-run until Ctrl-C, printing throughput every second
    ~/.pyenv/versions/3.11.11/envs/zephyr-env/bin/python3 ble_central.py --mode gatt --name Alif_B1_Test --duration 120
"""

import argparse
import sys
import time


def run_gatt_central(device_name, duration):
    """Receive GATT notifications via bleak."""
    import asyncio
    from bleak import BleakClient, BleakScanner

    # NUS TX UUID (same for both nRF and Alif throughput tests)
    TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

    rx_bytes = 0
    start_time = None

    def notification_handler(sender, data):
        nonlocal rx_bytes
        rx_bytes += len(data)

    async def run():
        nonlocal rx_bytes, start_time

        print(f"Scanning for '{device_name}'...", flush=True)
        device = await BleakScanner.find_device_by_name(device_name, timeout=15.0)
        if device is None:
            print(f"ERROR: Could not find '{device_name}'")
            return

        print(f"Found {device.name}, connecting...", flush=True)

        async with BleakClient(device) as client:
            print(f"Connected, MTU={client.mtu_size}", flush=True)
            await client.start_notify(TX_CHAR_UUID, notification_handler)

            start_time = time.time()
            prev_bytes = 0
            elapsed = 0

            while duration == 0 or elapsed < duration:
                await asyncio.sleep(1.0)
                now = time.time()
                elapsed = now - start_time
                delta = rx_bytes - prev_bytes
                prev_bytes = rx_bytes
                instant_kbps = (delta * 8) / 1000
                avg_kbps = (rx_bytes * 8) / 1000 / elapsed if elapsed > 0 else 0
                print(f"  [{elapsed:5.0f}s] {instant_kbps:6.0f} kbps (inst) "
                      f"{avg_kbps:6.0f} kbps (avg) | {rx_bytes:,} bytes", flush=True)

        if start_time:
            total = time.time() - start_time
            avg = (rx_bytes * 8) / 1000 / total if total > 0 else 0
            print(f"\n=== Final: {rx_bytes:,} bytes in {total:.1f}s = {avg:.0f} kbps ===")

    asyncio.run(run())


def run_l2cap_central(device_name, duration):
    """Receive L2CAP CoC data via CoreBluetooth (PyObjC)."""
    import struct
    from Foundation import NSObject, NSRunLoop, NSDate, NSDefaultRunLoopMode, NSTimer
    from CoreBluetooth import (
        CBCentralManager, CBManagerStatePoweredOn, CBUUID,
    )
    import objc

    PSM_SERVICE_UUID = "12345678-1234-5678-1234-56789ABCDEF0"
    PSM_CHAR_UUID = "12345678-1234-5678-1234-56789ABCDEF1"

    class L2CAPReceiver(NSObject):
        def init(self):
            self = objc.super(L2CAPReceiver, self).init()
            if self is None:
                return None
            self.target_name = device_name
            self.test_duration = duration
            self.peripheral = None
            self.l2cap_channels = []
            self.rx_bytes = 0
            self.start_time = None
            self.last_report_time = None
            self.last_report_bytes = 0
            self.scan_start_time = None
            self.central = CBCentralManager.alloc().initWithDelegate_queue_(self, None)
            return self

        def centralManagerDidUpdateState_(self, central):
            if central.state() == CBManagerStatePoweredOn:
                print(f"Scanning for '{self.target_name}'...", flush=True)
                self.scan_start_time = time.time()
                central.scanForPeripheralsWithServices_options_(None, None)

        def centralManager_didDiscoverPeripheral_advertisementData_RSSI_(
            self, central, peripheral, ad_data, rssi
        ):
            name = peripheral.name()
            if name and name == self.target_name:
                print(f"Found {name} (RSSI: {rssi})", flush=True)
                self.peripheral = peripheral
                central.stopScan()
                central.connectPeripheral_options_(peripheral, None)

        def centralManager_didConnectPeripheral_(self, central, peripheral):
            print(f"Connected to {peripheral.name()}", flush=True)
            peripheral.setDelegate_(self)
            peripheral.discoverServices_([CBUUID.UUIDWithString_(PSM_SERVICE_UUID)])

        def centralManager_didDisconnectPeripheral_error_(self, central, peripheral, error):
            print(f"Disconnected: {error}", flush=True)
            self._print_final()

        def peripheral_didDiscoverServices_(self, peripheral, error):
            if error:
                return
            for svc in peripheral.services():
                if svc.UUID().UUIDString().upper() == PSM_SERVICE_UUID.upper():
                    peripheral.discoverCharacteristics_forService_(
                        [CBUUID.UUIDWithString_(PSM_CHAR_UUID)], svc)

        def peripheral_didDiscoverCharacteristicsForService_error_(self, peripheral, service, error):
            if error:
                return
            for char in service.characteristics():
                if char.UUID().UUIDString().upper() == PSM_CHAR_UUID.upper():
                    peripheral.readValueForCharacteristic_(char)

        def peripheral_didUpdateValueForCharacteristic_error_(self, peripheral, char, error):
            if error:
                return
            data = char.value()
            if data is None or data.length() < 2:
                return
            raw = bytes(data)
            psm = struct.unpack_from("<H", raw, 0)[0]
            print(f"PSM: 0x{psm:04X}, opening L2CAP channel...", flush=True)
            peripheral.openL2CAPChannel_(psm)

        def peripheral_didOpenL2CAPChannel_error_(self, peripheral, channel, error):
            if error:
                print(f"L2CAP open error: {error}", flush=True)
                return
            self.l2cap_channels.append(channel)
            print("L2CAP channel opened!", flush=True)
            input_stream = channel.inputStream()
            input_stream.setDelegate_(self)
            input_stream.scheduleInRunLoop_forMode_(
                NSRunLoop.currentRunLoop(), NSDefaultRunLoopMode)
            input_stream.open()
            self.start_time = time.time()
            self.last_report_time = self.start_time
            self.last_report_bytes = 0

        def stream_handleEvent_(self, stream, event):
            if event == 2:  # NSStreamEventHasBytesAvailable
                buf = bytearray(65536)
                while stream.hasBytesAvailable():
                    result = stream.read_maxLength_(buf, 65536)
                    n = result[0] if isinstance(result, tuple) else result
                    if n > 0:
                        self.rx_bytes += n

        def printStats_(self, timer):
            if self.start_time is None:
                return
            now = time.time()
            elapsed = now - self.start_time
            if self.test_duration > 0 and elapsed >= self.test_duration:
                self._print_final()
                sys.exit(0)
            interval = now - self.last_report_time
            if interval < 0.9:
                return
            delta = self.rx_bytes - self.last_report_bytes
            kbps = (delta * 8) / (interval * 1000)
            avg_kbps = (self.rx_bytes * 8) / (elapsed * 1000) if elapsed > 0 else 0
            print(f"  [{elapsed:5.0f}s] {kbps:6.0f} kbps (inst) "
                  f"{avg_kbps:6.0f} kbps (avg) | {self.rx_bytes:,} bytes", flush=True)
            self.last_report_time = now
            self.last_report_bytes = self.rx_bytes

        def _print_final(self):
            if self.start_time is None:
                return
            total = time.time() - self.start_time
            avg = (self.rx_bytes * 8) / 1000 / total if total > 0 else 0
            print(f"\n=== Final: {self.rx_bytes:,} bytes in {total:.1f}s = {avg:.0f} kbps ===")

    receiver = L2CAPReceiver.alloc().init()
    NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(
        1.0, receiver, b"printStats:", None, True)

    print("Press Ctrl+C to stop\n", flush=True)
    try:
        run_loop = NSRunLoop.currentRunLoop()
        while True:
            run_loop.runMode_beforeDate_(
                NSDefaultRunLoopMode, NSDate.dateWithTimeIntervalSinceNow_(0.1))
            if (receiver.peripheral is None
                    and receiver.scan_start_time
                    and time.time() - receiver.scan_start_time > 15):
                print(f"Scan timeout - device not found.")
                sys.exit(1)
    except KeyboardInterrupt:
        print("\nStopping...")
        receiver._print_final()


def main():
    parser = argparse.ArgumentParser(description="BLE Central receiver for throughput tests")
    parser.add_argument("--mode", required=True, choices=["gatt", "l2cap"],
                        help="BLE protocol: gatt (notifications) or l2cap (CoC)")
    parser.add_argument("--name", required=True,
                        help="Device name to scan for (e.g. nRF54LM20_Test, Alif_B1_Test)")
    parser.add_argument("--duration", type=int, default=0,
                        help="Test duration in seconds (0 = run until Ctrl-C)")
    args = parser.parse_args()

    if args.mode == "gatt":
        run_gatt_central(args.name, args.duration)
    elif args.mode == "l2cap":
        run_l2cap_central(args.name, args.duration)


if __name__ == "__main__":
    main()
