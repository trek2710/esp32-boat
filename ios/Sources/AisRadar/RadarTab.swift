import SwiftUI

struct RadarTab: View {
    @ObservedObject var model: RadarModel
    @State private var autoScale = true
    @State private var manualRange = 2.0

    private let sweepTimer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    private var range: Double {
        guard autoScale, let olat = model.own.lat, let olon = model.own.lon else {
            return autoScale ? 2.0 : manualRange
        }
        var maxNm = 0.5
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

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                Circle().fill(model.connected ? .green : .orange).frame(width: 10, height: 10)
                Text(model.status).font(.caption)
                Spacer()
                Text("\(model.targets.count) tgt").font(.caption).foregroundStyle(.secondary)
            }
            HStack {
                Image(systemName: model.own.hasFix ? "location.fill" : "location")
                Text(posText).font(.caption.monospacedDigit())
                Text(model.own.hasFix ? "GPS" : "bench").font(.caption2).foregroundStyle(.secondary)
                Spacer()
            }
            .padding(.horizontal, 2)

            RadarView(own: model.own, targets: model.targets, rangeNm: range,
                      background: threatColor(model.own.threat))
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
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .background(threatColor(model.own.threat).ignoresSafeArea())
        .animation(.easeInOut(duration: 0.3), value: model.own.threat)
        .onReceive(sweepTimer) { _ in model.sweep() }
    }
}

func niceStep(_ t: Double) -> Double {
    for v in [0.5, 1, 2, 5, 10, 20, 50] where v >= t { return v }
    return 50
}
