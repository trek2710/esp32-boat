import Foundation

// Mirrors shared/ble/AisRadarBle.h. Decoded from the device's BLE notifies.

struct OwnShip {
    var lat: Double?
    var lon: Double?
    var cogDeg: Double?
    var sogKn: Double?           // own speed (for CPA); nil = n/a
    var hasFix: Bool = false
    var threat: Int = 0          // 0=none 1=safe 2=alert 3=danger (from device)
    var targetCount: Int = 0
    var batteryPct: Int?         // ESP battery %, nil = unknown / on USB
    var hasPosition: Bool { lat != nil && lon != nil }
}

struct AisTarget: Identifiable {
    let mmsi: UInt32
    var lat: Double
    var lon: Double
    var sogKn: Double?
    var cogDeg: Double?
    var shipType: UInt8
    var name: String
    var lastSeen: Date
    var id: UInt32 { mmsi }
}

struct DeviceSettings {
    var rangeCapNm: Int = 24
    var hideAnchored: Bool = true
    var depthThreshM: Int = 10       // chart deep-water cutoff (m)
    var chartLayers: UInt8 = 0x17    // bit0 coastline,1 land,2 depth,3 depcnt,4 TSS,5 buoys,6 lights
    var testTargets: Bool = true     // device injects the three demo AIS targets
    var phoneGps: Bool = true        // device prefers the phone's GPS as own ship
    var projMin: Int = 5             // course-projection stick length, minutes
}

// Chart layer bit positions (mirror CHART_* in shared/ble/AisRadarBle.h).
enum ChartLayer: UInt8 {
    case coastline = 0, land = 1, depth = 2, depcnt = 3, tss = 4, buoys = 5, lights = 6
}

@MainActor
final class RadarModel: ObservableObject {
    @Published var own = OwnShip()
    @Published var targets: [AisTarget] = []
    @Published var status: String = "Starting…"
    @Published var connected = false
    @Published var settings = DeviceSettings()

    // Set by the app — writes a 4-byte BleSettings to the device.
    var onWriteSettings: ((Data) -> Void)?

    func applySettings(_ d: Data) {
        guard d.count >= 2 else { return }
        settings.rangeCapNm = Int(d.u8(0))
        settings.hideAnchored = d.u8(1) != 0
        if d.count >= 4 {
            settings.depthThreshM = Int(d.u8(2))
            settings.chartLayers = d.u8(3)
        }
        if d.count >= 5 { settings.testTargets = d.u8(4) != 0 }
        if d.count >= 6 { settings.phoneGps = d.u8(5) != 0 }
        if d.count >= 7 { settings.projMin = max(1, Int(d.u8(6))) }
    }

    // Push the whole settings struct (the device persists + echoes it back).
    func pushSettings() {
        var d = Data()
        d.append(UInt8(max(1, min(255, settings.rangeCapNm))))
        d.append(settings.hideAnchored ? 1 : 0)
        d.append(UInt8(max(0, min(255, settings.depthThreshM))))
        d.append(settings.chartLayers)
        d.append(settings.testTargets ? 1 : 0)
        d.append(settings.phoneGps ? 1 : 0)
        d.append(UInt8(max(1, min(60, settings.projMin))))
        onWriteSettings?(d)
    }

    func layerOn(_ l: ChartLayer) -> Bool { settings.chartLayers & (1 << l.rawValue) != 0 }
    func setLayer(_ l: ChartLayer, _ on: Bool) {
        if on { settings.chartLayers |= (1 << l.rawValue) }
        else  { settings.chartLayers &= ~(1 << l.rawValue) }
        pushSettings()
    }

    // Client-side expiry, a touch longer than the device's 10 s lifetime so a
    // dropped notify doesn't blink a target out.
    private let lifetime: TimeInterval = 12

    func applyOwn(_ d: Data) {
        guard d.count >= 14 else { return }
        let cogRaw = d.i16le(8), sogRaw = d.i16le(10)
        own.lat = Double(d.i32le(0)) / 1e7
        own.lon = Double(d.i32le(4)) / 1e7
        own.cogDeg = (cogRaw == Int16.min) ? nil : Double(cogRaw) / 10.0
        own.sogKn = (sogRaw == Int16.min) ? nil : Double(sogRaw) / 10.0
        let flags = d.u8(12)
        own.hasFix = (flags & 0x01) != 0
        own.threat = Int((flags >> 1) & 0x03)
        own.targetCount = Int(d.u8(13))
        if d.count >= 15 { let b = d.u8(14); own.batteryPct = (b == 255) ? nil : Int(b) }
    }

    func applyTarget(_ d: Data) {
        guard d.count >= 38 else { return }
        let sogRaw = d.i16le(12), cogRaw = d.i16le(14)
        var nameBytes: [UInt8] = []
        for k in 18..<38 { let c = d.u8(k); if c == 0 { break }; nameBytes.append(c) }
        let t = AisTarget(
            mmsi: d.u32le(0),
            lat: Double(d.i32le(4)) / 1e7,
            lon: Double(d.i32le(8)) / 1e7,
            sogKn: (sogRaw == Int16.min) ? nil : Double(sogRaw) / 10.0,
            cogDeg: (cogRaw == Int16.min) ? nil : Double(cogRaw) / 10.0,
            shipType: d.u8(16),
            name: String(bytes: nameBytes, encoding: .ascii)?
                .trimmingCharacters(in: .whitespaces) ?? "",
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
