import SwiftUI

// AIS targets listed under the radar, sorted by threat then range. Mirrors the
// detail a chartplotter/converter shows: type, MMSI, motion, range/bearing,
// CPA/TCPA. (Names aren't on the BLE wire yet — type + MMSI identify a target.)
struct AisListView: View {
    let targets: [AisTarget]
    let own: OwnShip

    private var rows: [(t: AisTarget, ev: TargetEval)] {
        let evaluated = targets.map { (t: $0, ev: evaluateTarget($0, own: own)) }
        return evaluated.sorted { a, b in
            if a.ev.threat != b.ev.threat { return a.ev.threat > b.ev.threat }
            return a.ev.rangeNm < b.ev.rangeNm
        }
    }

    var body: some View {
        if targets.isEmpty {
            Text("No AIS targets")
                .font(.footnote).foregroundStyle(.secondary)
                .frame(maxWidth: .infinity).padding(.vertical, 10)
        } else {
            LazyVStack(spacing: 0) {
                ForEach(rows, id: \.t.mmsi) { row in
                    AisRow(t: row.t, ev: row.ev)
                    Divider()
                }
            }
        }
    }
}

struct AisRow: View {
    let t: AisTarget
    let ev: TargetEval

    var body: some View {
        HStack(spacing: 10) {
            Circle().fill(threatMarkColor(ev.threat)).frame(width: 10, height: 10)
            VStack(alignment: .leading, spacing: 2) {
                Text(t.name.isEmpty ? "MMSI \(t.mmsi)" : t.name)
                    .font(.subheadline.weight(.semibold))
                Text("\(shipTypeName(t.shipType)) · \(motion)")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text(String(format: "%.2f NM · %03.0f°", ev.rangeNm, ev.bearingDeg))
                    .font(.caption.monospacedDigit())
                Text(cpaText).font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 6).padding(.horizontal, 4)
    }

    private var motion: String {
        let sog = t.sogKn.map { String(format: "%.1f kn", $0) } ?? "– kn"
        let cog = t.cogDeg.map { String(format: "%03.0f°", $0) } ?? "   –"
        return "\(sog) · \(cog)"
    }
    private var cpaText: String {
        guard let cpa = ev.cpaNm, let tcpa = ev.tcpaSec, tcpa > 0 else { return "—" }
        return String(format: "CPA %.2f NM · %d:%02d", cpa, Int(tcpa) / 60, Int(tcpa) % 60)
    }
}
