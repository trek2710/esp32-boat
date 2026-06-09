import SwiftUI

struct RadarTab: View {
    @ObservedObject var model: RadarModel
    @State private var autoScale = true
    @State private var manualRange = 2.0
    @State private var chartStore = ChartStore()

    private let sweepTimer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    private var range: Double {
        guard autoScale, let olat = model.own.lat, let olon = model.own.lon else {
            return autoScale ? 2.0 : manualRange
        }
        var maxNm = model.own.hasPosition ? 1.0 : 0.5    // chart floor, like the device
        for t in model.targets {
            let (r, _) = rangeBearing(olat, olon, t.lat, t.lon)
            maxNm = max(maxNm, r)
        }
        return niceStep(maxNm * 1.15)
    }

    private var posText: String {
        guard let lat = model.own.lat, let lon = model.own.lon else { return "—" }
        return String(format: "%.5f, %.5f", lat, lon)
    }

    private var chart: [ChartFeature] {
        guard let lat = model.own.lat, let lon = model.own.lon else { return [] }
        return chartStore.features(forLat: lat, lon: lon)
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 8) {
                HStack(spacing: 8) {
                    Circle().fill(model.connected ? .green : .orange).frame(width: 10, height: 10)
                    Text(model.status).font(.caption)
                    Spacer()
                    if let b = model.own.batteryPct {
                        HStack(spacing: 3) {
                            Image(systemName: batterySymbol(b))
                            Text("\(b)%").font(.caption.monospacedDigit())
                        }
                        .foregroundStyle(b < 10 ? Color.red : Color.secondary)
                    }
                    Text("\(model.targets.count) tgt").font(.caption).foregroundStyle(.secondary)
                }
                HStack {
                    Image(systemName: model.own.hasFix ? "location.fill" : "location")
                    Text(posText).font(.caption.monospacedDigit())
                    Text(model.own.hasFix ? "GPS" : "bench").font(.caption2).foregroundStyle(.secondary)
                    Spacer()
                    if let sog = model.own.sogKn {
                        Image(systemName: "speedometer")
                        Text(String(format: "%.1f kn", sog)).font(.caption.monospacedDigit())
                    }
                }

                RadarView(own: model.own, targets: model.targets, rangeNm: range,
                          threat: model.own.threat, chart: chart,
                          depthThreshM: model.settings.depthThreshM,
                          chartLayers: model.settings.chartLayers,
                          projMin: model.settings.projMin)
                    .aspectRatio(1, contentMode: .fit)

                VStack(spacing: 4) {
                    Toggle("Auto scale", isOn: $autoScale).font(.subheadline)
                    if !autoScale {
                        HStack {
                            Text("Range").font(.caption)
                            Slider(value: $manualRange, in: 0.5...24, step: 0.5)
                            Text(String(format: "%.1f NM", manualRange))
                                .font(.caption).monospacedDigit().frame(width: 64, alignment: .trailing)
                        }
                    }
                }

                Divider().padding(.top, 4)
                AisListView(targets: model.targets, own: model.own)
            }
            .padding()
        }
        .onReceive(sweepTimer) { _ in model.sweep() }
    }
}

func niceStep(_ t: Double) -> Double {
    for v in [0.5, 0.75, 1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 48] where v >= t { return v }
    return 48
}

func batterySymbol(_ p: Int) -> String {
    switch p {
    case ..<13: return "battery.0"
    case ..<38: return "battery.25"
    case ..<63: return "battery.50"
    case ..<88: return "battery.75"
    default:    return "battery.100"
    }
}
