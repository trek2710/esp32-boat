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
}
