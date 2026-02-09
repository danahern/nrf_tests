import Foundation
import CoreBluetooth
import Observation

private let psmServiceUUID = CBUUID(string: "12345678-1234-5678-1234-56789ABCDEF0")
private let psmCharUUID    = CBUUID(string: "12345678-1234-5678-1234-56789ABCDEF1")

@Observable
final class BLEManager: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, StreamDelegate {
    var status: String = "Idle"
    var currentKbps: Double = 0
    var averageKbps: Double = 0
    var totalBytes: UInt64 = 0
    var elapsed: TimeInterval = 0
    var isConnected: Bool = false
    var logMessages: [String] = []

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var l2capChannel: CBL2CAPChannel?
    private var statsTimer: Timer?
    private var scanTimer: Timer?

    private var rxBytes: UInt64 = 0
    private var startTime: Date?
    private var lastReportTime: Date?
    private var lastReportBytes: UInt64 = 0

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    // MARK: - Public

    func start() {
        guard central.state == .poweredOn else {
            log("Bluetooth not ready (state: \(central.state.rawValue))")
            return
        }
        reset()
        status = "Scanning..."
        log("Scanning for nRF54L15_Test...")
        central.scanForPeripherals(withServices: nil)

        scanTimer = Timer.scheduledTimer(withTimeInterval: 15, repeats: false) { [weak self] _ in
            guard let self, self.peripheral == nil else { return }
            self.central.stopScan()
            self.status = "Scan timeout"
            self.log("Device not found after 15s")
        }
    }

    func disconnect() {
        if let p = peripheral {
            central.cancelPeripheralConnection(p)
        }
        cleanup()
        status = "Disconnected"
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        log("Bluetooth state: \(central.state.rawValue)")
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                         advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard peripheral.name == "nRF54L15_Test" else { return }
        log("Found \(peripheral.name ?? "?") (RSSI: \(RSSI))")
        scanTimer?.invalidate()
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        status = "Connecting..."
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        log("Connected")
        isConnected = true
        status = "Discovering services..."
        peripheral.discoverServices([psmServiceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        log("Connection failed: \(error?.localizedDescription ?? "unknown")")
        status = "Connection failed"
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        log("Disconnected\(error.map { ": \($0.localizedDescription)" } ?? "")")
        cleanup()
        status = "Disconnected"
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { log("Service discovery error: \(error.localizedDescription)"); return }
        guard let service = peripheral.services?.first(where: { $0.uuid == psmServiceUUID }) else {
            log("PSM service not found"); return
        }
        log("Found PSM service")
        peripheral.discoverCharacteristics([psmCharUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        if let error { log("Char discovery error: \(error.localizedDescription)"); return }
        guard let char = service.characteristics?.first(where: { $0.uuid == psmCharUUID }) else {
            log("PSM characteristic not found"); return
        }
        log("Reading PSM...")
        peripheral.readValue(for: char)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error { log("Read error: \(error.localizedDescription)"); return }
        guard let data = characteristic.value, data.count >= 2 else {
            log("Invalid PSM data"); return
        }
        let psm = data.withUnsafeBytes { $0.load(as: UInt16.self) }  // little-endian on ARM
        log("PSM = 0x\(String(psm, radix: 16, uppercase: true)) (\(psm))")
        status = "Opening L2CAP channel..."
        peripheral.openL2CAPChannel(CBL2CAPPSM(psm))
    }

    func peripheral(_ peripheral: CBPeripheral, didOpen channel: CBL2CAPChannel?, error: Error?) {
        if let error { log("L2CAP open error: \(error.localizedDescription)"); status = "Channel error"; return }
        guard let channel else { log("L2CAP channel is nil"); return }

        log("L2CAP channel opened!")
        l2capChannel = channel
        status = "Streaming"

        guard let input = channel.inputStream else {
            log("No input stream on channel"); return
        }
        input.delegate = self
        input.schedule(in: .current, forMode: .default)
        input.open()

        rxBytes = 0
        lastReportBytes = 0
        startTime = Date()
        lastReportTime = startTime
        totalBytes = 0

        statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.updateStats()
        }
    }

    // MARK: - StreamDelegate

    func stream(_ aStream: Stream, handle eventCode: Stream.Event) {
        guard eventCode == .hasBytesAvailable, let input = aStream as? InputStream else { return }
        let bufSize = 65536
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: bufSize)
        defer { buf.deallocate() }

        while input.hasBytesAvailable {
            let n = input.read(buf, maxLength: bufSize)
            if n > 0 {
                rxBytes += UInt64(n)
            }
        }
    }

    // MARK: - Private

    private func updateStats() {
        guard let start = startTime else { return }
        let now = Date()
        let totalElapsed = now.timeIntervalSince(start)
        elapsed = totalElapsed

        let interval = now.timeIntervalSince(lastReportTime ?? now)
        if interval > 0.1 {
            let delta = rxBytes - lastReportBytes
            currentKbps = Double(delta * 8) / (interval * 1000)
            lastReportTime = now
            lastReportBytes = rxBytes
        }

        totalBytes = rxBytes
        if totalElapsed > 0 {
            averageKbps = Double(rxBytes * 8) / (totalElapsed * 1000)
        }
    }

    private func cleanup() {
        statsTimer?.invalidate()
        statsTimer = nil
        scanTimer?.invalidate()
        scanTimer = nil

        if let input = l2capChannel?.inputStream {
            input.delegate = nil
            input.remove(from: .current, forMode: .default)
            input.close()
        }
        l2capChannel = nil
        peripheral = nil
        isConnected = false
    }

    private func reset() {
        cleanup()
        rxBytes = 0
        totalBytes = 0
        currentKbps = 0
        averageKbps = 0
        elapsed = 0
        startTime = nil
        lastReportTime = nil
        lastReportBytes = 0
        logMessages = []
    }

    private func log(_ msg: String) {
        let ts = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        logMessages.append("[\(ts)] \(msg)")
    }
}
