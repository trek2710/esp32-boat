// Tabs/AisMapView.swift — north-up AIS plan view (v1.6 step 6).
//
// Own ship sits at the centre; AIS targets are placed by their position
// relative to the own-ship GPS fix (equirectangular about the own
// latitude). Every vessel with a known SOG/COG gets a 5-minute
// forward-projection arrow. Pinch to zoom, drag to pan, tap the
// crosshair to recentre + auto-fit.
//
// Requires an own-ship fix (bus-published or iPhone GPS) — without an
// origin there's nothing to place targets around, so we show a
// placeholder instead. North-up: the chart never rotates; the own-ship
// icon rotates to show heading.

import SwiftUI
import CoreLocation

struct AisMapView: View {
    @EnvironmentObject var bus: BusState

    @State private var zoom: CGFloat = 1.0
    @GestureState private var pinch: CGFloat = 1.0
    @State private var pan: CGSize = .zero
    @GestureState private var drag: CGSize = .zero

    // 5-minute look-ahead for projection arrows.
    private let projectionSeconds = 300.0
    private let metersPerDegLat = 111_320.0
    // Targets quiet longer than this fade out (converter replays ~5 s).
    private let staleAfter: TimeInterval = 60.0

    var body: some View {
        // Own-ship origin: prefer the iPhone's own CoreLocation fix
        // (present whenever Location permission is granted, even when not
        // publishing onto the bus), and only fall back to a bus-published
        // GPS source. Fill motion from the iPhone fix when the bus has none.
        var own = bus.instruments
        if let fix = bus.location.lastFix {
            own.lat = fix.coordinate.latitude
            own.lon = fix.coordinate.longitude
            if own.cog.isNaN, fix.course >= 0 { own.cog = fix.course }
            if own.sog.isNaN, fix.speed  >= 0 { own.sog = fix.speed * 1.943844 }
        }
        return Group {
            if own.lat.isNaN || own.lon.isNaN {
                ContentUnavailableView(
                    "No GPS fix",
                    systemImage: "location.slash",
                    description: Text("The map centres on your iPhone's "
                        + "location. Allow Location access for this app in "
                        + "iOS Settings, or publish a GPS source onto the "
                        + "bus (sim.gps, or Share iPhone GPS)."))
            } else {
                mapCanvas(own: own)
            }
        }
    }

    private func mapCanvas(own: Instruments) -> some View {
        Canvas { ctx, size in
            draw(into: ctx, size: size, own: own, targets: bus.targets)
        }
        .gesture(
            SimultaneousGesture(
                MagnifyGesture()
                    .updating($pinch) { v, s, _ in s = v.magnification }
                    .onEnded { v in
                        zoom = min(max(zoom * v.magnification, 0.2), 20)
                    },
                DragGesture()
                    .updating($drag) { v, s, _ in s = v.translation }
                    .onEnded { v in
                        pan.width += v.translation.width
                        pan.height += v.translation.height
                    }
            )
        )
        .background(Color(.systemBackground))
        .clipped()
        .overlay(alignment: .bottomTrailing) {
            Button {
                zoom = 1.0; pan = .zero
            } label: {
                Image(systemName: "scope")
                    .font(.title2)
                    .padding(10)
                    .background(.thinMaterial, in: Circle())
            }
            .padding()
        }
    }

    // MARK: - drawing

    private func draw(into ctx: GraphicsContext, size: CGSize,
                      own: Instruments, targets: [AisTarget]) {
        let center = CGPoint(x: size.width / 2 + pan.width + drag.width,
                             y: size.height / 2 + pan.height + drag.height)
        let mPerDegLon = metersPerDegLat * cos(own.lat * .pi / 180)
        let valid = targets.filter { !$0.lat.isNaN && !$0.lon.isNaN }

        // east/north metres of a lat/lon relative to own.
        func en(_ lat: Double, _ lon: Double) -> (e: Double, n: Double) {
            ((lon - own.lon) * mPerDegLon, (lat - own.lat) * metersPerDegLat)
        }

        // Auto-fit: scale so the farthest target sits inside the ring,
        // floor at 1 NM so a lone own-ship still shows a sensible range.
        var maxDist = 1852.0
        for t in valid {
            let (e, n) = en(t.lat, t.lon)
            maxDist = max(maxDist, (e * e + n * n).squareRoot())
        }
        let radiusPx = max(40.0, Double(min(size.width, size.height)) / 2 - 48)
        let scale = (radiusPx / maxDist) * Double(zoom * pinch)  // px per metre

        func pt(_ e: Double, _ n: Double) -> CGPoint {
            CGPoint(x: center.x + e * scale, y: center.y - n * scale)
        }

        // ---- range rings ----
        let visibleRangeM = radiusPx / (scale == 0 ? 1 : scale)
        let stepM = niceStepMetres(visibleRangeM / 3)
        var ring = stepM
        while ring <= visibleRangeM * 1.05 {
            let r = ring * scale
            ctx.stroke(Path(ellipseIn: CGRect(x: center.x - r, y: center.y - r,
                                              width: r * 2, height: r * 2)),
                       with: .color(.gray.opacity(0.3)), lineWidth: 1)
            ctx.draw(Text(String(format: "%.2g NM", ring / 1852))
                        .font(.caption2).foregroundColor(.gray),
                     at: CGPoint(x: center.x, y: center.y - r - 8))
            ring += stepM
        }

        // ---- targets (projection arrow, then marker, then label) ----
        for t in valid {
            let (e, n) = en(t.lat, t.lon)
            let p = pt(e, n)
            let dim = Date().timeIntervalSince(t.lastSeen) > staleAfter
            let tint = Color.orange.opacity(dim ? 0.4 : 1.0)

            if let end = projectionEnd(from: p, sogKn: t.sog,
                                       cogDeg: t.cog, scale: scale) {
                drawArrow(ctx, from: p, to: end, color: tint, width: 2)
            }
            if !t.cog.isNaN {
                ctx.fill(vesselPath(at: p, headingDeg: t.cog, len: 9),
                         with: .color(tint))
            } else {
                let r = 5.0
                ctx.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r,
                                                width: r * 2, height: r * 2)),
                         with: .color(tint))
            }
            let label = t.name.isEmpty ? String(t.mmsi) : t.name
            ctx.draw(Text(label).font(.caption2)
                        .foregroundColor(.primary.opacity(dim ? 0.4 : 0.9)),
                     at: CGPoint(x: p.x, y: p.y + 16))
        }

        // ---- own ship (drawn last, on top) ----
        if let end = projectionEnd(from: center, sogKn: own.sog,
                                   cogDeg: own.cog, scale: scale) {
            drawArrow(ctx, from: center, to: end, color: .blue, width: 2.5)
        }
        let ownHeading = !own.headingMagDeg.isNaN ? own.headingMagDeg : own.cog
        ctx.fill(vesselPath(at: center, headingDeg: ownHeading.isNaN ? 0 : ownHeading,
                            len: 13),
                 with: .color(.blue))

        // ---- HUD (top-left, inside the canvas) ----
        let sog = own.sog.isNaN ? "—" : String(format: "%.1f kt", own.sog)
        let cog = own.cog.isNaN ? "—" : String(format: "%03.0f°", own.cog)
        let hud = "SOG \(sog)   COG \(cog)\nTargets \(valid.count)"
        ctx.draw(Text(hud).font(.caption.monospacedDigit())
                    .foregroundColor(.secondary),
                 at: CGPoint(x: 10, y: 12), anchor: .topLeading)
    }

    // MARK: - geometry helpers

    /// Screen point 5 min ahead of `p` for a vessel at sogKn/cogDeg, or
    /// nil if it's effectively stationary / unknown.
    private func projectionEnd(from p: CGPoint, sogKn: Double, cogDeg: Double,
                               scale: Double) -> CGPoint? {
        guard !sogKn.isNaN, !cogDeg.isNaN, sogKn > 0.1 else { return nil }
        let dist = sogKn * 0.514444 * projectionSeconds   // metres
        let cogR = cogDeg * .pi / 180
        return CGPoint(x: p.x + dist * sin(cogR) * scale,
                       y: p.y - dist * cos(cogR) * scale)
    }

    private func drawArrow(_ ctx: GraphicsContext, from a: CGPoint, to b: CGPoint,
                           color: Color, width: CGFloat) {
        var shaft = Path()
        shaft.move(to: a); shaft.addLine(to: b)
        ctx.stroke(shaft, with: .color(color), lineWidth: width)
        // arrowhead
        let ang = atan2(b.y - a.y, b.x - a.x)
        let h = 8.0, spread = 0.5
        var head = Path()
        head.move(to: b)
        head.addLine(to: CGPoint(x: b.x - h * cos(ang - spread),
                                 y: b.y - h * sin(ang - spread)))
        head.move(to: b)
        head.addLine(to: CGPoint(x: b.x - h * cos(ang + spread),
                                 y: b.y - h * sin(ang + spread)))
        ctx.stroke(head, with: .color(color), lineWidth: width)
    }

    /// A north-up vessel triangle rotated to `headingDeg` (compass).
    private func vesselPath(at c: CGPoint, headingDeg: Double, len: CGFloat) -> Path {
        let t = headingDeg * .pi / 180
        let cs = CGFloat(cos(t)), sn = CGFloat(sin(t))
        // local frame: x right, y forward. forward = (sin,−cos), right = (cos,sin).
        func map(_ lx: CGFloat, _ ly: CGFloat) -> CGPoint {
            CGPoint(x: c.x + lx * cs + ly * sn, y: c.y + lx * sn - ly * cs)
        }
        var p = Path()
        p.move(to: map(0, len))
        p.addLine(to: map(-len * 0.6, -len * 0.6))
        p.addLine(to: map(len * 0.6, -len * 0.6))
        p.closeSubpath()
        return p
    }

    /// Round a target ring spacing up to a chart-friendly value.
    private func niceStepMetres(_ target: Double) -> Double {
        let nm = target / 1852
        let steps = [0.1, 0.25, 0.5, 1, 2, 5, 10, 20, 50]
        let pick = steps.first { $0 >= nm } ?? 50
        return pick * 1852
    }
}
