// Tabs/BoatTab.swift — instrument dashboard mirror.
//
// v1 step 2 placeholder: shows the values BusState has parsed so far.
// Step 3 replaces the text grid with a SwiftUI Canvas compass widget
// (port of the LVGL geometry from src/Ui.cpp's buildMainPage).

import SwiftUI

struct BoatTab: View {
    @EnvironmentObject var bus: BusState

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                header
                grid
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
}
