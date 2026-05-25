// Tabs/BoatTab.swift — instrument dashboard mirror.
//
// v1 step 2 placeholder: shows the values BusState has parsed so far.
// Step 3 replaces the text grid with a SwiftUI Canvas compass widget
// (port of the LVGL geometry from src/Ui.cpp's buildMainPage).

import SwiftUI
import CoreLocation

struct BoatTab: View {
    @EnvironmentObject var bus: BusState

    /// Disagreement threshold between bus-published GPS and iPhone GPS,
    /// in metres. Per user spec (round 85 v1.6 step 4): if the bus
    /// position is more than 30 m off from the iPhone's, show it in
    /// bold orange as a "something's wrong" cue.
    private let kGpsDeltaWarnM: Double = 30.0

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                header
                grid
                positionSection
                Spacer().frame(height: 24)
            }
            .padding()
        }
        .navigationTitle("Boat")
    }

    private var header: some View {
        HStack {
            Image(systemName: "sailboat.fill").font(.title2)
            Text(bus.isOnline ? "Online" : "Offline")
                .foregroundStyle(bus.isOnline ? .green : .secondary)
            Spacer()
            if let err = bus.lastError {
                Text(err).font(.caption).foregroundStyle(.red)
            }
        }
    }

    private var grid: some View {
        LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 18) {
            cell("TWA", angle(bus.instruments.twa))
            cell("TWS", speed(bus.instruments.tws))
            cell("AWA", angle(bus.instruments.awa))
            cell("AWS", speed(bus.instruments.aws))
            cell("BSP", speed(bus.instruments.stw))
            cell("HDG", angle(bus.instruments.headingMagDeg))
            cell("DPT", depth(bus.instruments.depthM))
            cell("VMG", speed(bus.instruments.vmg))
        }
    }

    private func cell(_ caption: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(caption).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.system(.title, design: .rounded).weight(.semibold))
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func angle(_ v: Double) -> String { v.isNaN ? "—" : String(format: "%+5.0f°", v) }
    private func speed(_ v: Double) -> String { v.isNaN ? "—" : String(format: "%4.1f kt", v) }
    private func depth(_ v: Double) -> String { v.isNaN ? "—" : String(format: "%4.1f m",  v) }

    // MARK: - Position cross-check
    //
    // Shows the bus-published GPS prominently. When the iPhone has its
    // own fix, computes the great-circle distance to the bus position;
    // > kGpsDeltaWarnM (30 m) flips the bus value to bold orange so a
    // glance reveals "the bus thinks we're somewhere different from
    // where my phone thinks we are."

    private var positionSection: some View {
        let bLat = bus.instruments.lat
        let bLon = bus.instruments.lon
        let hasBus = !bLat.isNaN && !bLon.isNaN
        let iosFix = bus.location.lastFix
        let deltaM: Double? = {
            guard hasBus, let f = iosFix else { return nil }
            let busLoc = CLLocation(latitude: bLat, longitude: bLon)
            return busLoc.distance(from: f)
        }()
        let warn = (deltaM ?? 0) > kGpsDeltaWarnM

        return VStack(alignment: .leading, spacing: 8) {
            Text("POSITION").font(.caption).foregroundStyle(.secondary)

            // Bus position (bold orange when iPhone disagrees by > 30 m).
            HStack(spacing: 8) {
                Text("Bus")
                    .font(.caption2).foregroundStyle(.secondary)
                    .frame(width: 50, alignment: .leading)
                if hasBus {
                    Text(formatLatLon(bLat, bLon))
                        .font(.system(.body, design: .monospaced))
                        .fontWeight(warn ? .bold : .regular)
                        .foregroundStyle(warn ? .orange : .primary)
                } else {
                    Text("—").foregroundStyle(.secondary)
                }
            }

            // iPhone position (always plain — it's the reference).
            HStack(spacing: 8) {
                Text("iPhone")
                    .font(.caption2).foregroundStyle(.secondary)
                    .frame(width: 50, alignment: .leading)
                if let f = iosFix {
                    Text(formatLatLon(f.coordinate.latitude,
                                      f.coordinate.longitude))
                        .font(.system(.body, design: .monospaced))
                        .foregroundStyle(.secondary)
                } else {
                    Text(bus.location.isAuthorized ? "waiting for fix…"
                                                   : "not authorized")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            // Delta line — only when both fixes are available.
            if let d = deltaM {
                HStack(spacing: 8) {
                    Text("Δ")
                        .font(.caption2).foregroundStyle(.secondary)
                        .frame(width: 50, alignment: .leading)
                    Text(formatDistance(d))
                        .font(.callout)
                        .fontWeight(warn ? .bold : .regular)
                        .foregroundStyle(warn ? .orange : .secondary)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func formatLatLon(_ lat: Double, _ lon: Double) -> String {
        String(format: "%9.5f°, %9.5f°", lat, lon)
    }

    private func formatDistance(_ m: Double) -> String {
        if m < 1000 { return String(format: "%.0f m off", m) }
        if m < 100_000 { return String(format: "%.2f km off", m / 1000) }
        return String(format: "%.0f km off", m / 1000)
    }
}
