import SwiftUI

struct ContentView: View {
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

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Circle().fill(model.connected ? .green : .orange).frame(width: 10, height: 10)
                Text(model.status).font(.caption)
                Spacer()
                if let fix = model.own.lat != nil ? model.own.hasFix : nil {
                    Text(fix ? "GPS" : "bench").font(.caption2).foregroundStyle(.secondary)
                }
                Text("\(model.targets.count) tgt").font(.caption).foregroundStyle(.secondary)
            }
            .padding(.horizontal)

            RadarView(own: model.own, targets: model.targets, rangeNm: range)
                .aspectRatio(1, contentMode: .fit)
                .padding(.horizontal, 4)

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
            .padding(.horizontal)
            .padding(.bottom, 8)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.black)
        .preferredColorScheme(.dark)
        .onReceive(sweepTimer) { _ in model.sweep() }
    }
}

func niceStep(_ t: Double) -> Double {
    for v in [0.5, 1, 2, 5, 10, 20, 50] where v >= t { return v }
    return 50
}
