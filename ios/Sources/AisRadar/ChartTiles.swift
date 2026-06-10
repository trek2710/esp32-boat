import Foundation

// Reads the pre-baked CM93 chart tiles (.c93t, produced by tools/chart-transcode
// and bundled under Charts/) for the own-ship cell. Mirrors the device's
// ChartOverlay; the heavy decode happened offline.

struct ChartFeature {
    let layer: UInt8
    let isArea: Bool
    let depth: Float            // DRVAL1/VALDCO metres; NaN if n/a
    let pts: [SIMD2<Double>]    // (lat, lon)
}

final class ChartStore {
    private var loaded: [UInt32: [ChartFeature]] = [:]
    private var lastKey = ""
    private var merged: [ChartFeature] = []

    // Own cell + any neighbour within ~0.6° of an edge (matches the device), so
    // the chart stays continuous across the 4° tile boundaries.
    static func neededCells(_ lat: Double, _ lon: Double) -> [UInt32] {
        let dv = 12.0
        var lon1 = (lon + 360.0) * 3.0
        while lon1 >= 1080.0 { lon1 -= 1080.0 }
        let lon3 = Int((lon1 / dv).rounded(.down)) * 12
        let lat1 = lat * 3.0 + 240.0
        let lat3 = Int((lat1 / dv).rounded(.down)) * 12
        let flat = (lat1 - Double(lat3)) / 3.0, flon = (lon1 - Double(lon3)) / 3.0
        let m = 0.6
        var dlas = [0]; if flat < m { dlas.append(-12) } else if flat > 4 - m { dlas.append(12) }
        var dlos = [0]; if flon < m { dlos.append(-12) } else if flon > 4 - m { dlos.append(12) }
        var out: [UInt32] = []
        for a in dlas { for b in dlos {
            let la = lat3 + a
            var lo = lon3 + b
            while lo < 0 { lo += 1080 }
            while lo >= 1080 { lo -= 1080 }
            out.append(UInt32((la + 30) * 10000 + lo))
        }}
        return out
    }

    func features(forLat lat: Double, lon: Double) -> [ChartFeature] {
        let need = Self.neededCells(lat, lon)
        let key = need.sorted().map(String.init).joined(separator: ",")
        if key == lastKey { return merged }
        lastKey = key
        for id in need where loaded[id] == nil { loaded[id] = Self.load(cellId: id) }
        let needSet = Set(need)
        for id in Array(loaded.keys) where !needSet.contains(id) { loaded[id] = nil }
        merged = need.flatMap { loaded[$0] ?? [] }
        return merged
    }

    private static func load(cellId: UInt32) -> [ChartFeature] {
        let name = String(format: "%08u", cellId)
        guard let url = Bundle.main.url(forResource: name, withExtension: "c93t"),
              let data = try? Data(contentsOf: url) else { return [] }
        return parse([UInt8](data))
    }

    // Header (28 B): u32 magic 'C93T' | u8 ver | u8 nlayers | u16 pad |
    //                4×f32 bbox | u32 feat_count
    // Feature: u8 layer | u8 flags(bit0=area) | f32 depth | u16 npts |
    //          npts×(f32 lat, f32 lon)   — all little-endian.
    private static func parse(_ b: [UInt8]) -> [ChartFeature] {
        func u16(_ o: Int) -> Int { Int(b[o]) | Int(b[o + 1]) << 8 }
        func u32(_ o: Int) -> UInt32 {
            UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24
        }
        func f32(_ o: Int) -> Float { Float(bitPattern: u32(o)) }
        guard b.count >= 28, u32(0) == 0x5433_3943 else { return [] }
        let count = Int(u32(24))
        var off = 28
        var out: [ChartFeature] = []
        out.reserveCapacity(count)
        for _ in 0..<count {
            guard off + 8 <= b.count else { break }
            let layer = b[off], flags = b[off + 1]
            let depth = f32(off + 2)
            let npts = u16(off + 6)
            off += 8
            guard off + npts * 8 <= b.count else { break }
            var pts: [SIMD2<Double>] = []
            pts.reserveCapacity(npts)
            for i in 0..<npts {
                pts.append(SIMD2(Double(f32(off + i * 8)), Double(f32(off + i * 8 + 4))))
            }
            off += npts * 8
            out.append(ChartFeature(layer: layer, isArea: (flags & 1) != 0,
                                    depth: depth, pts: pts))
        }
        return out
    }
}
