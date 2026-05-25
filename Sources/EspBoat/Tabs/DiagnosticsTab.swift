// Tabs/DiagnosticsTab.swift — read-only "is the bus alive?" view.
//
// v1 step 2 placeholder: shows what BusState has so far (peer count,
// last error, settings version). Step 6 grows this into peers list +
// WiFi + PGN rates + raw-log drawer per docs/IOS_APP.md.

import SwiftUI

struct DiagnosticsTab: View {
    @EnvironmentObject var bus: BusState

    var body: some View {
        Form {
            Section("Connection") {
                LabeledContent("Status",
                               value: bus.isOnline ? "Online" : "Offline")
                    .foregroundStyle(bus.isOnline ? .green : .secondary)
                LabeledContent("AP host",  value: Bus.apHost)
                LabeledContent("UDP port", value: String(Bus.busPort))
                if let err = bus.lastError {
                    LabeledContent("Last error", value: err)
                        .foregroundStyle(.red)
                }
            }

            Section("Settings") {
                LabeledContent("Version", value: "\(bus.snapshot.settingsV)")
            }

            Section("iPhone GPS (publish)") {
                LabeledContent("Status", value: gpsStatusText)
                    .foregroundStyle(gpsStatusColor)
                if let fix = bus.location.lastFix {
                    LabeledContent("Position",
                                   value: String(format: "%.5f, %.5f",
                                                 fix.coordinate.latitude,
                                                 fix.coordinate.longitude))
                    let ageS = Int(-fix.timestamp.timeIntervalSinceNow)
                    LabeledContent("Last fix", value: "\(ageS) s ago")
                }
                LabeledContent("Published",
                               value: "\(bus.location.didSendCount)")
            }

            Section("Peers") {
                if bus.peerNames.isEmpty {
                    Text("none yet (UDP listener arrives in next iteration)")
                        .foregroundStyle(.secondary)
                        .font(.caption)
                } else {
                    ForEach(bus.peerNames, id: \.self) { Text($0) }
                }
            }
        }
        .navigationTitle("Diagnostics")
    }

    private var gpsStatusText: String {
        if !bus.location.isAuthorized { return "permission needed" }
        if bus.location.lastFix == nil { return "waiting for fix…" }
        return "publishing"
    }
    private var gpsStatusColor: Color {
        if !bus.location.isAuthorized { return .orange }
        if bus.location.lastFix == nil { return .secondary }
        return .green
    }
}
