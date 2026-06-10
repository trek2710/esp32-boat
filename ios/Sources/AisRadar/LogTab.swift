import SwiftUI

// Raw dAISy NMEA stream from the device — to see whether the receiver is
// emitting anything at all (keep-alives + AIS), and what's filtered.
struct LogTab: View {
    @ObservedObject var model: RadarModel

    private var aivdmCount: Int { model.log.filter { $0.contains("AIVD") }.count }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                HStack(spacing: 8) {
                    Image(systemName: model.own.daisyAlive ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash")
                        .foregroundStyle(model.own.daisyAlive ? .green : .secondary)
                    Text(model.own.daisyAlive ? "dAISy active" : "no dAISy data")
                        .font(.subheadline.weight(.semibold))
                    Spacer()
                    Text("\(model.log.count) lines · \(aivdmCount) AIVDM")
                        .font(.caption).foregroundStyle(.secondary)
                }
                .padding(.horizontal).padding(.vertical, 8)
                Divider()

                if model.log.isEmpty {
                    Spacer()
                    Text("No dAISy lines received.\nIf this stays empty, the receiver isn't sending — check power/wiring.")
                        .multilineTextAlignment(.center).font(.footnote).foregroundStyle(.secondary)
                        .padding()
                    Spacer()
                } else {
                    ScrollViewReader { proxy in
                        ScrollView {
                            LazyVStack(alignment: .leading, spacing: 2) {
                                ForEach(Array(model.log.enumerated()), id: \.offset) { i, line in
                                    Text(line)
                                        .font(.system(size: 11, design: .monospaced))
                                        .foregroundStyle(line.contains("AIVD") ? .green : .secondary)
                                        .textSelection(.enabled)
                                        .frame(maxWidth: .infinity, alignment: .leading)
                                        .id(i)
                                }
                            }.padding(8)
                        }
                        .onChange(of: model.log.count) { _, n in
                            if n > 0 { withAnimation { proxy.scrollTo(n - 1, anchor: .bottom) } }
                        }
                    }
                }
            }
            .navigationTitle("dAISy log")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Clear") { model.log.removeAll() }.disabled(model.log.isEmpty)
                }
            }
        }
    }
}
