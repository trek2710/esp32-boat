import Foundation

// Mirrors shared/ble/AisRadarBle.h. Decoded from the device's BLE notifies.

struct OwnShip {
    var lat: Double?
    var lon: Double?
    var cogDeg: Double?
    var hasFix: Bool = false
    var threat: Int = 0          // 0=none 1=safe 2=alert 3=danger (from device)
    var targetCount: Int = 0
    var hasPosition: Bool { lat != nil && lon != nil }
}

struct AisTarget: Identifiable {
    let mmsi: UInt32
    var lat: Double
    var lon: Double
    var sogKn: Double?
    var cogDeg: Double?
    var shipType: UInt8
    var lastSeen: Date
    var id: UInt32 { mmsi }
}

struct DeviceSettings {
    var rangeCapNm: Int = 24
    var hideAnchored: Bool = true
}

@MainActor
final class RadarModel: ObservableObject {
    @Published var own = OwnShip()
    @Published var targets: [AisTarget] = []
    @Published var status: String = "Starting…"
    @Published var connected = false
    @Published var settings = DeviceSettings()

    // Set by the app — writes a 2-byte BleSettings to the device.
    var onWriteSettings: ((Data) -> Void)?

    func applySettings(_ d: Data) {
        guard d.count >= 2 else { return }
        settings.rangeCapNm = Int(d.u8(0))
        settings.hideAnchored = d.u8(1) != 0
    }

    func writeSettings(rangeCapNm: Int, hideAnchored: Bool) {
        var d = Data()
        d.append(UInt8(max(1, min(255, rangeCapNm))))
        d.append(hideAnchored ? 1 : 0)
        onWriteSettings?(d)
    }

    // Client-side expiry, a touch longer than the device's 10 s lifetime so a
    // dropped notify doesn't blink a target out.
    private let lifetime: TimeInterval = 12

    func applyOwn(_ d: Data) {
        guard d.count >= 12 else { return }
        let lat = Double(d.i32le(0)) / 1e7
        let lon = Double(d.i32le(4)) / 1e7
        let cogRaw = d.i16le(8)
        own.lat = lat
        own.lon = lon
        own.cogDeg = (cogRaw == Int16.min) ? nil : Double(cogRaw) / 10.0
        let flags = d.u8(10)
        own.hasFix = (flags & 0x01) != 0
        own.threat = Int((flags >> 1) & 0x03)
        own.targetCount = Int(d.u8(11))
    }

    func applyTarget(_ d: Data) {
        guard d.count >= 18 else { return }
        let sogRaw = d.i16le(12)
        let cogRaw = d.i16le(14)
        let t = AisTarget(
            mmsi: d.u32le(0),
            lat: Double(d.i32le(4)) / 1e7,
            lon: Double(d.i32le(8)) / 1e7,
            sogKn: (sogRaw == Int16.min) ? nil : Double(sogRaw) / 10.0,
            cogDeg: (cogRaw == Int16.min) ? nil : Double(cogRaw) / 10.0,
            shipType: d.u8(16),
            lastSeen: Date())
        if let i = targets.firstIndex(where: { $0.mmsi == t.mmsi }) { targets[i] = t }
        else { targets.append(t) }
    }

    func sweep() {
        let now = Date()
        targets.removeAll { now.timeIntervalSince($0.lastSeen) > lifetime }
    }
}

// Little-endian byte readers over a BLE Data payload.
extension Data {
    func u8(_ i: Int) -> UInt8 { self[startIndex + i] }
    func u32le(_ i: Int) -> UInt32 {
        UInt32(u8(i)) | UInt32(u8(i + 1)) << 8 | UInt32(u8(i + 2)) << 16 | UInt32(u8(i + 3)) << 24
    }
    func i32le(_ i: Int) -> Int32 { Int32(bitPattern: u32le(i)) }
    func i16le(_ i: Int) -> Int16 { Int16(bitPattern: UInt16(u8(i)) | UInt16(u8(i + 1)) << 8) }
}
