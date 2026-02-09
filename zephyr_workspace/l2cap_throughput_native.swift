#!/usr/bin/env swift
//
// Native Swift L2CAP CoC Throughput Test
// Eliminates Python/PyObjC overhead to test true macOS BLE ceiling.
// Compile: swiftc -O -o l2cap_test l2cap_throughput_native.swift
// Run: ./l2cap_test
//

import Foundation
import CoreBluetooth

// Flush stdout after every print (Swift buffers when piped)
func log(_ msg: String) {
    Swift.print(msg)
    fflush(stdout)
}

let PSM_SERVICE_UUID = CBUUID(string: "12345678-1234-5678-1234-56789ABCDEF0")
let PSM_CHAR_UUID = CBUUID(string: "12345678-1234-5678-1234-56789ABCDEF1")

class L2CAPTest: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, StreamDelegate {
    var central: CBCentralManager!
    var peripheral: CBPeripheral?
    var channel: CBL2CAPChannel?

    var rxBytes: UInt64 = 0
    var startTime: Date?
    var lastReportTime: Date?
    var lastReportBytes: UInt64 = 0
    var scanStartTime: Date?

    // Pre-allocated buffer — no malloc/free per read
    let bufSize = 65536
    var buf: UnsafeMutablePointer<UInt8>

    override init() {
        buf = .allocate(capacity: 65536)
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    deinit {
        buf.deallocate()
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            log("Bluetooth powered on, scanning...")
            scanStartTime = Date()
            central.scanForPeripherals(withServices: nil)
        } else {
            log("Bluetooth not available (state: \(central.state.rawValue))")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                         advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard peripheral.name == "nRF54L15_Test" else { return }
        log("Found \(peripheral.name!) (RSSI: \(RSSI))")
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("Connected")
        peripheral.discoverServices([PSM_SERVICE_UUID])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        log("Disconnected\(error.map { ": \($0.localizedDescription)" } ?? "")")
        printFinalStats()
        exit(0)
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil else { log("Service error: \(error!)"); return }
        guard let svc = peripheral.services?.first(where: { $0.uuid == PSM_SERVICE_UUID }) else {
            log("PSM service not found"); return
        }
        log("Found PSM service")
        peripheral.discoverCharacteristics([PSM_CHAR_UUID], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard error == nil else { log("Char error: \(error!)"); return }
        guard let char = service.characteristics?.first(where: { $0.uuid == PSM_CHAR_UUID }) else {
            log("PSM char not found"); return
        }
        peripheral.readValue(for: char)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil else { log("Read error: \(error!)"); return }
        guard let data = characteristic.value, data.count >= 2 else { log("Invalid PSM"); return }
        let psm: UInt16 = data.withUnsafeBytes { $0.load(as: UInt16.self) }
        log("PSM = 0x\(String(psm, radix: 16, uppercase: true)) (\(psm))")
        log("Opening L2CAP channel...")
        peripheral.openL2CAPChannel(CBL2CAPPSM(psm))
    }

    func peripheral(_ peripheral: CBPeripheral, didOpen channel: CBL2CAPChannel?, error: Error?) {
        guard error == nil else { log("L2CAP error: \(error!)"); return }
        guard let channel else { log("Channel nil"); return }

        log("L2CAP channel opened!")
        self.channel = channel

        channel.inputStream.delegate = self
        channel.inputStream.schedule(in: .current, forMode: .default)
        channel.inputStream.open()

        rxBytes = 0
        lastReportBytes = 0
        startTime = Date()
        lastReportTime = startTime
        log("Receiving data...")
    }

    // MARK: - StreamDelegate (hot path — keep minimal)

    func stream(_ aStream: Stream, handle eventCode: Stream.Event) {
        guard eventCode == .hasBytesAvailable, let input = aStream as? InputStream else { return }
        while input.hasBytesAvailable {
            let n = input.read(buf, maxLength: bufSize)
            if n > 0 { rxBytes += UInt64(n) }
        }
    }

    // MARK: - Stats

    @objc func printStats() {
        guard let start = startTime else {
            // Check scan timeout
            if let scanStart = scanStartTime, Date().timeIntervalSince(scanStart) > 15 {
                log("\nScan timeout after 15s - device not found.")
                exit(1)
            }
            return
        }

        let now = Date()
        let elapsed = now.timeIntervalSince(start)
        let interval = now.timeIntervalSince(lastReportTime ?? now)
        guard interval >= 0.5 else { return }

        let delta = rxBytes - lastReportBytes
        let kbps = Double(delta * 8) / (interval * 1000)
        let avgKbps = elapsed > 0 ? Double(rxBytes * 8) / (elapsed * 1000) : 0

        log("RX: \(Int(kbps)) kbps (avg: \(Int(avgKbps)) kbps) | \(rxBytes) bytes in \(String(format: "%.1f", elapsed))s")

        lastReportTime = now
        lastReportBytes = rxBytes
    }

    func printFinalStats() {
        guard let start = startTime else { return }
        let elapsed = Date().timeIntervalSince(start)
        guard elapsed > 0 else { return }
        let avgKbps = Double(rxBytes * 8) / (elapsed * 1000)
        log("\n=== Final Stats ===")
        log("Duration: \(String(format: "%.1f", elapsed))s")
        log("Total RX: \(rxBytes) bytes")
        log("Average: \(Int(avgKbps)) kbps")
    }
}

// Main
log("Native Swift L2CAP Throughput Test")
log("Scanning for 'nRF54L15_Test'...\n")

let test = L2CAPTest()

Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { _ in
    test.printStats()
}

signal(SIGINT) { _ in
    log("\nStopping...")
    exit(0)
}

RunLoop.current.run()
