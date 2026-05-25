// Tabs/AisTab.swift — AIS targets list.
//
// v1 step 2 placeholder: empty list with onboarding text. Step 4
// renders BusState.targets sorted by range when own-GPS is known,
// otherwise by last-seen; tap → push detail view.

import SwiftUI

struct AisTab: View {
    @EnvironmentObject var bus: BusState

    var body: some View {
        Group {
            if bus.targets.isEmpty {
                emptyState
            } else {
                List(bus.targets) { target in
                    AisRow(target: target)
                }
            }
        }
        .navigationTitle("AIS")
    }

    private var emptyState: some View {
        VStack(spacing: 12) {
            Image(systemName: "dot.radiowaves.left.and.right")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text("No AIS targets").foregroundStyle(.secondary)
            Text("Pair with the boat's WiFi and wait for the converter "
                 + "to replay its target cache.")
                .multilineTextAlignment(.center)
                .font(.caption)
                .foregroundStyle(.tertiary)
                .padding(.horizontal, 32)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct AisRow: View {
    let target: AisTarget
    var body: some View {
        VStack(alignment: .leading) {
            Text(target.name.isEmpty ? String(target.mmsi) : target.name)
                .font(.headline)
            HStack(spacing: 12) {
                if !target.sog.isNaN {
                    Label(String(format: "%.1f kt", target.sog),
                          systemImage: "speedometer")
                }
                if !target.cog.isNaN {
                    Label(String(format: "%.0f°", target.cog),
                          systemImage: "location.north.line")
                }
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
    }
}
