import SwiftUI

struct SettingsTab: View {
    @ObservedObject var model: RadarModel

    private var posText: String {
        guard let lat = model.own.lat, let lon = model.own.lon else { return "—" }
        return String(format: "%.5f, %.5f", lat, lon)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("ais-radar device") {
                    row("Connection", model.connected ? "● \(model.status)" : "○ \(model.status)")
                    row("Own position", posText)
                    row("Position source", model.own.hasFix ? "GPS" : "bench")
                    row("Targets", "\(model.targets.count)")
                }

                Section {
                    HStack {
                        Text("Range cap")
                        Spacer()
                        Text("\(model.settings.rangeCapNm) NM").foregroundStyle(.secondary)
                        Stepper("", value: rangeBinding, in: 1...48).labelsHidden()
                    }
                    Toggle("Hide anchored / moored", isOn: hideAnchoredBinding)
                } header: {
                    Text("AIS filters")
                } footer: {
                    Text("Applied on the device and the app. Targets beyond the range cap, or anchored/moored/aground, are hidden.")
                }

                Section {
                    Toggle("Coastline", isOn: layerBinding(.coastline))
                    Toggle("Depth shading", isOn: layerBinding(.depth))
                    HStack {
                        Text("Shallow depth")
                        Spacer()
                        Text("< \(model.settings.depthThreshM) m").foregroundStyle(.secondary)
                    }
                    Slider(value: depthBinding, in: 0...30, step: 1)
                } header: {
                    Text("Chart overlay")
                } footer: {
                    Text("Vector chart drawn under the radar. Depth areas shallower than the threshold are tinted; slide to change it live.")
                }
            }
            .navigationTitle("Settings")
            .disabled(!model.connected)
        }
    }

    private func layerBinding(_ l: ChartLayer) -> Binding<Bool> {
        Binding(get: { model.layerOn(l) }, set: { model.setLayer(l, $0) })
    }
    private var depthBinding: Binding<Double> {
        Binding(get: { Double(model.settings.depthThreshM) },
                set: { model.settings.depthThreshM = Int($0); model.pushSettings() })
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label)
            Spacer()
            Text(value).foregroundStyle(.secondary).font(.callout.monospacedDigit())
        }
    }

    // Edits write to the device; it echoes the applied value back via notify,
    // which updates model.settings — so the bindings read from the model.
    private var rangeBinding: Binding<Int> {
        Binding(get: { model.settings.rangeCapNm },
                set: { model.settings.rangeCapNm = $0; model.pushSettings() })
    }
    private var hideAnchoredBinding: Binding<Bool> {
        Binding(get: { model.settings.hideAnchored },
                set: { model.settings.hideAnchored = $0; model.pushSettings() })
    }
}
