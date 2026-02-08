#!/usr/bin/env python3
"""
L2CAP CoC Throughput Test for nRF54L15

Uses PyObjC CoreBluetooth directly (bleak doesn't support L2CAP CoC).
Connects to the nRF54L15, reads the PSM from GATT, opens an L2CAP
channel, and measures RX throughput.

Requirements:
    pip install pyobjc-framework-CoreBluetooth

Usage:
    python3 l2cap_throughput_test.py
    python3 l2cap_throughput_test.py --name nRF54L15_L2CAP
    python3 l2cap_throughput_test.py --duration 30
"""

import argparse
import struct
import time
import sys
import threading
from Foundation import (
    NSObject, NSRunLoop, NSDate, NSDefaultRunLoopMode, NSTimer,
)
from CoreBluetooth import (
    CBCentralManager, CBPeripheral,
    CBManagerStatePoweredOn,
    CBCharacteristicPropertyRead,
    CBUUID,
)
import objc

# PSM Discovery Service UUIDs (must match firmware)
PSM_SERVICE_UUID = "12345678-1234-5678-1234-56789ABCDEF0"
PSM_CHAR_UUID = "12345678-1234-5678-1234-56789ABCDEF1"


class L2CAPThroughputTest(NSObject):

    def init(self):
        self = objc.super(L2CAPThroughputTest, self).init()
        if self is None:
            return None

        self.target_name = "nRF54L15_L2CAP"
        self.duration = 0  # 0 = run forever
        self.peripheral = None
        self.psm = None
        self.l2cap_channel = None

        # Stats
        self.rx_bytes = 0
        self.start_time = None
        self.last_report_time = None
        self.last_report_bytes = 0

        self.central = CBCentralManager.alloc().initWithDelegate_queue_(self, None)
        return self

    # -- CBCentralManagerDelegate --

    def centralManagerDidUpdateState_(self, central):
        if central.state() == CBManagerStatePoweredOn:
            print("Bluetooth powered on, scanning...")
            central.scanForPeripheralsWithServices_options_(None, None)
        else:
            print(f"Bluetooth not available (state={central.state()})")

    def centralManager_didDiscoverPeripheral_advertisementData_RSSI_(
        self, central, peripheral, ad_data, rssi
    ):
        name = peripheral.name()
        if name and name == self.target_name:
            print(f"Found {name} (RSSI: {rssi})")
            self.peripheral = peripheral
            central.stopScan()
            central.connectPeripheral_options_(peripheral, None)

    def centralManager_didConnectPeripheral_(self, central, peripheral):
        print(f"Connected to {peripheral.name()}")
        peripheral.setDelegate_(self)
        peripheral.discoverServices_([CBUUID.UUIDWithString_(PSM_SERVICE_UUID)])

    def centralManager_didFailToConnectPeripheral_error_(self, central, peripheral, error):
        print(f"Failed to connect: {error}")

    def centralManager_didDisconnectPeripheral_error_(self, central, peripheral, error):
        print(f"Disconnected: {error}")
        self.l2cap_channel = None
        self._print_final_stats()

    # -- CBPeripheralDelegate --

    def peripheral_didDiscoverServices_(self, peripheral, error):
        if error:
            print(f"Service discovery error: {error}")
            return

        for service in peripheral.services():
            if service.UUID().UUIDString().upper() == PSM_SERVICE_UUID.upper():
                print(f"Found PSM service")
                peripheral.discoverCharacteristics_forService_(
                    [CBUUID.UUIDWithString_(PSM_CHAR_UUID)], service
                )

    def peripheral_didDiscoverCharacteristicsForService_error_(
        self, peripheral, service, error
    ):
        if error:
            print(f"Characteristic discovery error: {error}")
            return

        for char in service.characteristics():
            if char.UUID().UUIDString().upper() == PSM_CHAR_UUID.upper():
                print("Found PSM characteristic, reading...")
                peripheral.readValueForCharacteristic_(char)

    def peripheral_didUpdateValueForCharacteristic_error_(
        self, peripheral, characteristic, error
    ):
        if error:
            print(f"Read error: {error}")
            return

        data = characteristic.value()
        if data is None or data.length() < 2:
            print("Invalid PSM data")
            return

        self.psm = struct.unpack_from("<H", bytes(data))[0]
        print(f"PSM = 0x{self.psm:04X} ({self.psm})")
        print("Opening L2CAP channel...")
        peripheral.openL2CAPChannel_(self.psm)

    def peripheral_didOpenL2CAPChannel_error_(self, peripheral, channel, error):
        if error:
            print(f"L2CAP channel open error: {error}")
            return

        print("L2CAP channel opened!")
        self.l2cap_channel = channel

        input_stream = channel.inputStream()
        input_stream.setDelegate_(self)
        input_stream.scheduleInRunLoop_forMode_(
            NSRunLoop.currentRunLoop(), NSDefaultRunLoopMode
        )
        input_stream.open()

        self.start_time = time.time()
        self.last_report_time = self.start_time
        self.last_report_bytes = 0
        self.rx_bytes = 0

        print("Receiving data...")

    # -- NSStreamDelegate --

    def stream_handleEvent_(self, stream, event):
        # NSStreamEventHasBytesAvailable = 2
        if event == 2:
            buf_size = 65536
            buf = bytearray(buf_size)
            while stream.hasBytesAvailable():
                read = stream.read_maxLength_(buf, buf_size)
                if read > 0:
                    self.rx_bytes += read

    # -- Stats Timer --

    def printStats_(self, timer):
        if self.start_time is None:
            return

        now = time.time()
        interval = now - self.last_report_time
        if interval < 0.5:
            return

        delta = self.rx_bytes - self.last_report_bytes
        kbps = (delta * 8) / (interval * 1000)
        elapsed = now - self.start_time
        avg_kbps = (self.rx_bytes * 8) / (elapsed * 1000) if elapsed > 0 else 0

        print(f"RX: {kbps:.0f} kbps (avg: {avg_kbps:.0f} kbps) | "
              f"{self.rx_bytes:,} bytes in {elapsed:.1f}s")

        self.last_report_time = now
        self.last_report_bytes = self.rx_bytes

        if self.duration > 0 and elapsed >= self.duration:
            self._print_final_stats()
            print("Duration reached, stopping.")
            sys.exit(0)

    def _print_final_stats(self):
        if self.start_time is None:
            return
        elapsed = time.time() - self.start_time
        if elapsed > 0:
            avg_kbps = (self.rx_bytes * 8) / (elapsed * 1000)
            print(f"\n=== Final Stats ===")
            print(f"Duration: {elapsed:.1f}s")
            print(f"Total RX: {self.rx_bytes:,} bytes")
            print(f"Average:  {avg_kbps:.0f} kbps")


def main():
    parser = argparse.ArgumentParser(description="L2CAP CoC Throughput Test")
    parser.add_argument(
        "--name", default="nRF54L15_L2CAP",
        help="Device name to scan for (default: nRF54L15_L2CAP)"
    )
    parser.add_argument(
        "--duration", type=int, default=0,
        help="Test duration in seconds (0 = run forever)"
    )
    args = parser.parse_args()

    test = L2CAPThroughputTest.alloc().init()
    test.target_name = args.name
    test.duration = args.duration

    # Schedule stats timer (fires every 1s)
    NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(
        1.0, test, b"printStats:", None, True
    )

    print(f"Scanning for '{args.name}'...")
    print("Press Ctrl+C to stop\n")

    try:
        run_loop = NSRunLoop.currentRunLoop()
        while True:
            run_loop.runMode_beforeDate_(
                NSDefaultRunLoopMode, NSDate.dateWithTimeIntervalSinceNow_(0.1)
            )
    except KeyboardInterrupt:
        print("\nStopping...")
        test._print_final_stats()


if __name__ == "__main__":
    main()
