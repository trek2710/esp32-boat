// Networking/BusState.swift — long-lived observable owning the UDP
// receive listener (BusListener), the 5-second heartbeat publisher
// (HeartbeatPublisher), and the settings-snapshot mirror.
//
// Lifecycle: start() on app foreground, stop() on background. Heartbeats
// must fire continuously so the AP's g_peers fanout table keeps our IP
// fresh (entries expire after 30 s on the firmware side).

import Foundation
import Combine

@MainActor
final class BusState: ObservableObject {
    // Live published state — Boat / AIS / Diagnostics tabs observe.
    @Published var instruments: Instruments = Instruments()
    @Published var targets: [AisTarget]     = []
    @Published var snapshot: SettingsSnapshot = SettingsSnapshot(
        settingsV: 0, settings: BoatSettings())
    @Published var isOnline: Bool = false
    @Published var lastError: String? = nil

    // Last-known peer table from heartbeats (excludes us).
    @Published var peerNames: [String] = []
    @Published var apPeer: String = ""

    // Last successful packet timestamp — used by Diagnostics + Boat.
    @Published var lastPacketAt: Date? = nil

    private let settingsClient = SettingsClient()
    private let heartbeat = HeartbeatPublisher()
    private var listener: BusListener?

    // MARK: - lifecycle
    func start() {
        // Receive listener first so we don't miss the first fan-out
        // burst after the AP learns our IP from our heartbeat.
        do {
            let l = BusListener { [weak self] pgn, raw in
                self?.onPacket(pgn: pgn, raw: raw)
            }
            try l.start()
            listener = l
        } catch {
            self.lastError = "listener: \(error.localizedDescription)"
        }
        heartbeat.start()
        isOnline = true
        Task { await refreshSettings() }
    }

    func stop() {
        listener?.stop(); listener = nil
        heartbeat.stop()
        isOnline = false
    }

    // MARK: - packet dispatch
    private func onPacket(pgn: Int, raw: String) {
        let _ = PgnDispatch.handle(
            pgn: pgn, json: raw,
            instruments: &instruments,
            targets: &targets,
            peers: &peerNames,
            settings: &snapshot)
        // Round 85 v1.6 step 3: mirror BoatState::recomputeDerived_locked()
        // — TWA / TWS / VMG are derived locally, not on the wire.
        recomputeDerived()
        // Always touch lastPacketAt so the Boat/Diagnostics tabs can
        // show a heartbeat-alive indicator.
        lastPacketAt = Date()
        // Any successful receive clears stale transport errors.
        if lastError != nil { lastError = nil }
    }

    /// Compute TWA / TWS / VMG from the current AWA / AWS / STW. If any
    /// input is NaN the derived fields go NaN, matching BoatState's
    /// behaviour: "true wind needs apparent wind and boat speed."
    private func recomputeDerived() {
        let awa = instruments.awa
        let aws = instruments.aws
        let bsp = instruments.stw
        guard !awa.isNaN, !aws.isNaN, !bsp.isNaN else {
            instruments.twa = .nan
            instruments.tws = .nan
            instruments.twd = .nan
            instruments.vmg = .nan
            return
        }
        let awaR = awa * .pi / 180.0
        // Boat-frame triangle: true_wind = apparent_wind - boat_velocity.
        // Boat heads down +x at BSP knots; AWA is measured from bow,
        // positive starboard. Same convention as BoatState.cpp.
        let twx = aws * cos(awaR) - bsp
        let twy = aws * sin(awaR)
        let twaR = atan2(twy, twx)
        instruments.twa = twaR * 180.0 / .pi
        instruments.tws = sqrt(twx * twx + twy * twy)
        // VMG = boat speed component along the true-wind axis.
        instruments.vmg = bsp * cos(twaR)
        // TWD = TWA + heading (true). HDG is magnetic; we don't have
        // variation applied yet on iOS so leave TWD NaN for now.
        instruments.twd = .nan
    }

    // MARK: - HTTP settings
    func refreshSettings() async {
        do {
            let s = try await settingsClient.fetch()
            self.snapshot = s
            self.lastError = nil
        } catch {
            self.lastError = error.localizedDescription
        }
    }

    func apply<E: Encodable>(_ key: String, _ value: E) async {
        do {
            let s = try await settingsClient.apply([key: AnyEncodable(value)])
            self.snapshot = s
            self.lastError = nil
        } catch {
            self.lastError = error.localizedDescription
        }
    }
}
