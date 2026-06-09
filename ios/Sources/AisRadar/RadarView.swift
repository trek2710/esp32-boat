import SwiftUI

// North-up PPI radar with the CM93 chart underneath — mirrors the AMOLED:
// sand land, depth-banded blues over white deep water, coastline + shipping
// lanes, range rings, N/E/S/W, AIS targets (icon by type, colour by threat),
// magenta own ship, and the threat as a ring around the rim.
struct RadarView: View {
    let own: OwnShip
    let targets: [AisTarget]
    let rangeNm: Double
    let threat: Int            // worst threat from the device (ring colour)
    let chart: [ChartFeature]
    let depthThreshM: Int
    let chartLayers: UInt8
    let projMin: Int

    private let deepWater = Color(hex: 0xE9F1F7)
    private let sand      = Color(hex: 0xE6D9A8)
    private let coastline = Color(hex: 0x35434F)
    private let tssGrey   = Color(hex: 0x8C949C)
    private let ringColor = Color(hex: 0x6E7A86)
    private let ownColor  = Color(hex: 0xC00060)
    private let labelC    = Color(hex: 0x0E1C28)

    private func on(_ l: ChartLayer) -> Bool { chartLayers & (1 << l.rawValue) != 0 }

    private func depthColor(_ drval: Float) -> Color? {
        if drval.isNaN || drval >= Float(depthThreshM) { return nil }   // offshore = white
        if drval <= 1 { return Color(hex: 0xCFE6F5) }
        if drval <= 3 { return Color(hex: 0xA9CFEC) }
        if drval <= 6 { return Color(hex: 0x7DB2DF) }
        return Color(hex: 0x5491CB)
    }

    var body: some View {
        Canvas { ctx, size in
            let cx = size.width / 2, cy = size.height / 2
            let outer = min(size.width, size.height) / 2 - 2
            let ringW: Double = threat > 0 ? 14 : 0
            let r = outer - ringW
            let center = CGPoint(x: cx, y: cy)

            if threat > 0 {
                ctx.fill(disc(center, outer), with: .color(threatRingColor(threat)))
            }
            ctx.fill(disc(center, r), with: .color(deepWater))

            guard let olat = own.lat, let olon = own.lon else {
                ctx.draw(Text("No position").foregroundColor(labelC), at: center)
                return
            }
            let scale = r / rangeNm
            let coslat = cos(olat * .pi / 180)
            let clip = rangeNm * 1.25
            func proj(_ la: Double, _ lo: Double) -> CGPoint {
                CGPoint(x: cx + (lo - olon) * 60 * coslat * scale,
                        y: cy - (la - olat) * 60 * scale)
            }

            // Everything below is clipped to the radar circle.
            var cc = ctx
            cc.clip(to: disc(center, r))

            // Pass 1a — depth bands (bottom). 1b — land on top of the water.
            if on(.depth) {
                for f in chart where f.isArea && f.layer == ChartLayer.depth.rawValue {
                    guard let c = depthColor(f.depth),
                          let path = featurePath(f, clip, olat, olon, coslat, proj, close: true)
                    else { continue }
                    cc.fill(path, with: .color(c))
                }
            }
            if on(.land) {
                for f in chart where f.isArea && f.layer == ChartLayer.land.rawValue {
                    guard let path = featurePath(f, clip, olat, olon, coslat, proj, close: true)
                    else { continue }
                    cc.fill(path, with: .color(sand))
                }
            }
            // Pass 2 — coastline + shipping lanes on top.
            for f in chart {
                let col: Color?
                if f.layer == ChartLayer.coastline.rawValue, on(.coastline) { col = coastline }
                else if f.layer == ChartLayer.tss.rawValue, on(.tss) { col = tssGrey }
                else { col = nil }
                guard let c = col,
                      let path = featurePath(f, clip, olat, olon, coslat, proj, close: false)
                else { continue }
                cc.stroke(path, with: .color(c), lineWidth: 1)
            }

            // Range rings + compass.
            for k in 1...3 {
                cc.stroke(disc(center, r * Double(k) / 3), with: .color(ringColor), lineWidth: 1)
            }
            let lf = Font.system(size: 15, weight: .bold)
            cc.draw(Text("N").font(lf).foregroundColor(labelC), at: CGPoint(x: cx, y: cy - r + 12))
            cc.draw(Text("S").font(lf).foregroundColor(labelC), at: CGPoint(x: cx, y: cy + r - 12))
            cc.draw(Text("E").font(lf).foregroundColor(labelC), at: CGPoint(x: cx + r - 12, y: cy))
            cc.draw(Text("W").font(lf).foregroundColor(labelC), at: CGPoint(x: cx - r + 12, y: cy))
            cc.draw(Text(String(format: "%.3g NM", rangeNm)).font(lf).foregroundColor(labelC),
                    at: CGPoint(x: cx, y: cy - r + 30))

            // AIS targets.
            for t in targets {
                let ev = evaluateTarget(t, own: own)
                if ev.rangeNm > rangeNm { continue }
                let p = proj(t.lat, t.lon)
                let col = threatMarkColor(ev.threat)
                if let sog = t.sogKn, let cog = t.cogDeg, sog > 0.1 {
                    let d = sog * (Double(projMin) / 60.0)
                    let cr = cog * .pi / 180
                    var stick = Path()
                    stick.move(to: p)
                    stick.addLine(to: CGPoint(x: p.x + d * scale * sin(cr), y: p.y - d * scale * cos(cr)))
                    cc.stroke(stick, with: .color(col), lineWidth: 1.5)
                }
                cc.fill(iconPath(t, at: p), with: .color(col))
                cc.draw(Text(targetLabel(t)).font(.system(size: 11, weight: .semibold))
                            .foregroundColor(labelC),
                        at: CGPoint(x: p.x + 10, y: p.y - 9), anchor: .leading)
            }

            // Own ship: course projection (same time base as targets) + speed.
            if let sog = own.sogKn, let cog = own.cogDeg, sog > 0.1 {
                let d = sog * (Double(projMin) / 60.0)
                let cr = cog * .pi / 180
                var stick = Path()
                stick.move(to: center)
                stick.addLine(to: CGPoint(x: cx + d * scale * sin(cr), y: cy - d * scale * cos(cr)))
                cc.stroke(stick, with: .color(ownColor), lineWidth: 2)
            }
            cc.fill(vesselPath(x: cx, y: cy, headingDeg: own.cogDeg ?? 0, len: 11), with: .color(ownColor))
            if let sog = own.sogKn {
                cc.draw(Text(String(format: "%.1f kn", sog))
                            .font(.system(size: 14, weight: .bold)).foregroundColor(ownColor),
                        at: CGPoint(x: cx, y: cy + 22))
            }
        }
    }

    private func iconPath(_ t: AisTarget, at p: CGPoint) -> Path {
        let hdg = t.cogDeg ?? 0
        switch shipTypeIcon(t.shipType) {
        case .circle:   return Path(ellipseIn: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10))
        case .hull:     return hullPath(x: p.x, y: p.y, headingDeg: hdg, len: 9)
        case .triangle: return vesselPath(x: p.x, y: p.y, headingDeg: hdg, len: 8)
        }
    }
}

private func disc(_ c: CGPoint, _ r: Double) -> Path {
    Path(ellipseIn: CGRect(x: c.x - r, y: c.y - r, width: r * 2, height: r * 2))
}

// Screen-space path for a chart feature; nil if nothing is in range.
private func featurePath(_ f: ChartFeature, _ clip: Double,
                         _ olat: Double, _ olon: Double, _ coslat: Double,
                         _ proj: (Double, Double) -> CGPoint, close: Bool) -> Path? {
    var any = false
    for q in f.pts where abs((q.x - olat) * 60) < clip && abs((q.y - olon) * 60 * coslat) < clip {
        any = true; break
    }
    guard any, f.pts.count >= (close ? 3 : 2) else { return nil }
    var path = Path()
    path.move(to: proj(f.pts[0].x, f.pts[0].y))
    for i in 1..<f.pts.count { path.addLine(to: proj(f.pts[i].x, f.pts[i].y)) }
    if close { path.closeSubpath() }
    return path
}

func threatRingColor(_ level: Int) -> Color {
    switch level {
    case 1:  return Color(hex: 0x12B021)
    case 2:  return Color(hex: 0xC8C800)
    case 3:  return Color(hex: 0xE00000)
    default: return Color(hex: 0xE9F1F7)
    }
}

// elongated hull (cargo/tanker), bow forward along heading
func hullPath(x: Double, y: Double, headingDeg: Double, len: Double) -> Path {
    let t = headingDeg * .pi / 180, cs = cos(t), sn = sin(t)
    func map(_ lx: Double, _ ly: Double) -> CGPoint {
        CGPoint(x: x + lx * cs + ly * sn, y: y + lx * sn - ly * cs)
    }
    var p = Path()
    p.move(to: map(0, len * 1.3))
    p.addLine(to: map(len * 0.5, len * 0.3))
    p.addLine(to: map(len * 0.5, -len))
    p.addLine(to: map(-len * 0.5, -len))
    p.addLine(to: map(-len * 0.5, len * 0.3))
    p.closeSubpath()
    return p
}

extension Color {
    init(hex: UInt32) {
        self.init(red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255)
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
