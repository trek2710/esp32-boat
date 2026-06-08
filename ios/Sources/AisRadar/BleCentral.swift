import CoreBluetooth
import Foundation

// BLE central: scans for the ais-radar device, connects, subscribes to the
// own-ship + target notify characteristics, and feeds the RadarModel.
// UUIDs mirror shared/ble/AisRadarBle.h.

final class BleCentral: NSObject {
    static let svcUUID = CBUUID(string: "a15a0001-7a11-4b3c-8d2e-0f1a2b3c4d5e")
    static let ownUUID = CBUUID(string: "a15a0002-7a11-4b3c-8d2e-0f1a2b3c4d5e")
    static let tgtUUID = CBUUID(string: "a15a0003-7a11-4b3c-8d2e-0f1a2b3c4d5e")

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private let model: RadarModel

    init(model: RadarModel) {
        self.model = model
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    private func setStatus(_ s: String, connected: Bool? = nil) {
        Task { @MainActor in
            model.status = s
            if let c = connected { model.connected = c }
        }
    }

    private func startScan() {
        central.scanForPeripherals(withServices: [Self.svcUUID])
        setStatus("Scanning for ais-radar…", connected: false)
    }
}

extension BleCentral: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn: startScan()
        case .poweredOff: setStatus("Bluetooth is off", connected: false)
        case .unauthorized: setStatus("Bluetooth not permitted", connected: false)
        default: setStatus("Bluetooth unavailable", connected: false)
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        peripheral = p
        p.delegate = self
        c.stopScan()
        setStatus("Connecting…")
        c.connect(p)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        setStatus("Discovering services…")
        p.discoverServices([Self.svcUUID])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        startScan()
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        setStatus("Disconnected — rescanning…", connected: false)
        startScan()
    }
}

extension BleCentral: CBPeripheralDelegate {
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        for s in p.services ?? [] where s.uuid == Self.svcUUID {
            p.discoverCharacteristics([Self.ownUUID, Self.tgtUUID], for: s)
        }
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for ch in s.characteristics ?? [] {
            if ch.uuid == Self.ownUUID || ch.uuid == Self.tgtUUID {
                p.setNotifyValue(true, for: ch)
            }
        }
        setStatus("Connected", connected: true)
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        guard let d = ch.value else { return }
        Task { @MainActor in
            if ch.uuid == Self.ownUUID { model.applyOwn(d) }
            else if ch.uuid == Self.tgtUUID { model.applyTarget(d) }
        }
    }
}
