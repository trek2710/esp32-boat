// Tabs/SettingsTab.swift — the actual write path against the AP.
//
// This tab is the whole point of the iOS app: a real keyboard, real
// form controls, sub-second feedback. On every change we POST a single
// key to http://192.168.4.1/settings; the AP echoes back the canonical
// snapshot with the bumped settings_v, which we adopt locally.
//
// All edits are optimistic-after-echo (we wait for the response before
// updating local state). The nav-bar title shows the live version
// number so the user can see their change land.

import SwiftUI

struct SettingsTab: View {
    @EnvironmentObject var bus: BusState

    var body: some View {
        Form {
            Section("Simulator") {
                Toggle("Wind",     isOn: bind(\.simWind,    key: "sim.wind"))
                Toggle("GPS",      isOn: bind(\.simGps,     key: "sim.gps"))
                Toggle("Heading",  isOn: bind(\.simHeading, key: "sim.heading"))
                Toggle("Depth/STW", isOn: bind(\.simDepth,  key: "sim.depth"))
                Toggle("Sea temp", isOn: bind(\.simSeaTemp, key: "sim.sea_temp"))
                Toggle("Air temp", isOn: bind(\.simAirTemp, key: "sim.air_temp"))
            }

            Section("Display") {
                Stepper(value: bind(\.uiBrightness, key: "ui.brightness"),
                        in: 0...100, step: 10) {
                    LabeledContent("Brightness",
                                   value: "\(bus.snapshot.settings.uiBrightness) %")
                }
                Picker("Idle dim after",
                       selection: bind(\.uiIdleDimAfterS, key: "ui.idle_dim_after_s")) {
                    Text("30 s").tag(30)
                    Text("1 min").tag(60)
                    Text("5 min").tag(300)
                    Text("15 min").tag(900)
                    Text("never").tag(0)
                }
            }

            Section("Navigation") {
                Stepper(value: bind(\.navNoGoDeg, key: "nav.no_go_deg"),
                        in: 20...60, step: 5) {
                    LabeledContent("No-go-zone half-angle",
                                   value: "±\(bus.snapshot.settings.navNoGoDeg)°")
                }
            }

            Section("AIS filters") {
                Stepper(value: bind(\.aisRangeNm, key: "ais.range_nm"),
                        in: 1...48, step: 1) {
                    LabeledContent("Range cap",
                                   value: "\(bus.snapshot.settings.aisRangeNm) NM")
                }
                Toggle("Hide anchored / moored",
                       isOn: bind(\.aisHideAnchored, key: "ais.hide_anchored"))
                Picker("Stale after",
                       selection: bind(\.aisStaleS, key: "ais.stale_s")) {
                    Text("1 min").tag(60)
                    Text("5 min").tag(300)
                    Text("10 min").tag(600)
                    Text("30 min").tag(1800)
                    Text("1 hour").tag(3600)
                }
            }

            Section("About") {
                LabeledContent("AP version", value: "\(bus.snapshot.settingsV)")
                LabeledContent("AP host",    value: Bus.apHost)
                if let err = bus.lastError {
                    LabeledContent("Last error",
                                   value: err).foregroundStyle(.red)
                }
                Button("Refresh") {
                    Task { await bus.refreshSettings() }
                }
            }
        }
        .navigationTitle("Settings v\(bus.snapshot.settingsV)")
        .refreshable { await bus.refreshSettings() }
    }

    // Helper: a Binding that reads from the current snapshot and, on
    // commit, POSTs a single key/value to the AP.
    private func bind<V: Encodable & Equatable>(
        _ kp: KeyPath<BoatSettings, V>, key: String
    ) -> Binding<V> {
        Binding(
            get: { bus.snapshot.settings[keyPath: kp] },
            set: { newValue in
                Task { await bus.apply(key, newValue) }
            }
        )
    }
}
