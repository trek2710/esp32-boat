import SwiftUI

// North-up PPI radar: own ship centred, AIS targets at range/bearing with
// 5-min COG projection sticks. Mirrors the on-device radar.
struct RadarView: View {
    let own: OwnShip
    let targets: [AisTarget]
    let rangeNm: Double
    let background: Color

    // Neutral so they read on any threat background.
    private let ringColor = Color(white: 0.35)
    private let blip = Color.white
    private let dim  = Color(white: 0.5)
    private let ownColor = Color(red: 0.0, green: 0.90, blue: 1.0)
    private let labelColor = Color(white: 0.75)

    var body: some View {
        Canvas { ctx, size in
            let cx = size.width / 2
            let cy = size.height / 2
            let r = min(size.width, size.height) / 2 - 28

            ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(background))
            for k in 1...3 {
                let rr = r * Double(k) / 3
                ctx.stroke(Path(ellipseIn: CGRect(x: cx - rr, y: cy - rr, width: rr * 2, height: rr * 2)),
                           with: .color(ringColor), lineWidth: 1)
            }
            var nTick = Path()
            nTick.move(to: CGPoint(x: cx, y: cy - r))
            nTick.addLine(to: CGPoint(x: cx, y: cy - r + 14))
            ctx.stroke(nTick, with: .color(ringColor), lineWidth: 1.5)
            ctx.draw(Text("N").font(.caption2).foregroundColor(labelColor),
                     at: CGPoint(x: cx, y: cy - r - 7))
            ctx.draw(Text(String(format: "%.3g NM", rangeNm)).font(.caption2).foregroundColor(labelColor),
                     at: CGPoint(x: cx, y: cy + r + 12))

            guard let olat = own.lat, let olon = own.lon else {
                ctx.draw(Text("No position").foregroundColor(labelColor), at: CGPoint(x: cx, y: cy))
                return
            }
            let scale = r / rangeNm

            for t in targets {
                let (rng, brg) = rangeBearing(olat, olon, t.lat, t.lon)
                if rng > rangeNm { continue }
                let br = brg * .pi / 180
                let x = cx + rng * scale * sin(br)
                let y = cy - rng * scale * cos(br)
                let stale = Date().timeIntervalSince(t.lastSeen) > 6
                let col = stale ? dim : blip

                if let sog = t.sogKn, let cog = t.cogDeg, sog > 0.1 {
                    let d = sog * (300.0 / 3600.0)        // NM in 5 min
                    let cr = cog * .pi / 180
                    var stick = Path()
                    stick.move(to: CGPoint(x: x, y: y))
                    stick.addLine(to: CGPoint(x: x + d * scale * sin(cr), y: y - d * scale * cos(cr)))
                    ctx.stroke(stick, with: .color(col), lineWidth: 1.5)
                }
                ctx.fill(vesselPath(x: x, y: y, headingDeg: t.cogDeg ?? 0, len: 7), with: .color(col))
                ctx.draw(Text(String(t.mmsi % 100000)).font(.system(size: 9)).foregroundColor(labelColor),
                         at: CGPoint(x: x + 16, y: y))
            }

            ctx.fill(vesselPath(x: cx, y: cy, headingDeg: own.cogDeg ?? 0, len: 11), with: .color(ownColor))
        }
        .background(background)
    }
}

// Whole-screen threat background: dark → green → amber → red.
func threatColor(_ level: Int) -> Color {
    switch level {
    case 1: return Color(red: 0.024, green: 0.20, blue: 0.06)   // green
    case 2: return Color(red: 0.48, green: 0.48, blue: 0.0)     // yellow (R=G)
    case 3: return Color(red: 0.478, green: 0.0, blue: 0.0)     // red
    default: return .black
    }
}

// great-circle range (NM) + initial bearing (deg true)
func rangeBearing(_ lat1: Double, _ lon1: Double, _ lat2: Double, _ lon2: Double) -> (Double, Double) {
    let R = 3440.065, d2r = Double.pi / 180
    let phi1 = lat1 * d2r, phi2 = lat2 * d2r
    let dphi = (lat2 - lat1) * d2r, dlam = (lon2 - lon1) * d2r
    let a = sin(dphi / 2) * sin(dphi / 2) + cos(phi1) * cos(phi2) * sin(dlam / 2) * sin(dlam / 2)
    let rng = R * 2 * atan2(sqrt(a), sqrt(1 - a))
    let y = sin(dlam) * cos(phi2)
    let x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlam)
    var b = atan2(y, x) * 180 / Double.pi
    if b < 0 { b += 360 }
    return (rng, b)
}

// north-up triangle rotated to a compass heading
func vesselPath(x: Double, y: Double, headingDeg: Double, len: Double) -> Path {
    let t = headingDeg * Double.pi / 180, cs = cos(t), sn = sin(t)
    func map(_ lx: Double, _ ly: Double) -> CGPoint {
        CGPoint(x: x + lx * cs + ly * sn, y: y + lx * sn - ly * cs)
    }
    var p = Path()
    p.move(to: map(0, len))
    p.addLine(to: map(-len * 0.6, -len * 0.6))
    p.addLine(to: map(len * 0.6, -len * 0.6))
    p.closeSubpath()
    return p
}
