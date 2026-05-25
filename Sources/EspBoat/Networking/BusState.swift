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
    private var staleTimer: AnyCancellable?
    /// How long since a channel's last-seen before we NaN-clear it. Match
    /// the firmware-side 3 s window used by BoatState::invalidateLiveData
    /// and the TX-side per-channel staleness check.
    private let kStaleAfter: TimeInterval = 3.0

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
        // Periodic stale sweep — blanks instrument fields whose source
        // channel has stopped publishing for > kStaleAfter.
        staleTimer = Timer.publish(every: 1.0, on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in self?.sweepStale() }
        Task { await refreshSettings() }
    }

    func stop() {
        listener?.stop(); listener = nil
        heartbeat.stop()
        staleTimer?.cancel(); staleTimer = nil
        isOnline = false
    }

    /// NaN-clear any instrument whose source channel hasn't produced an
    /// update within kStaleAfter. Triggers @Published so the Boat tab
    /// re-renders with "—" placeholders.
    private func sweepStale() {
        let now = Date()
        let stale = { (ts: Date?) -> Bool in
            guard let ts else { return false }
            return now.timeIntervalSince(ts) > self.kStaleAfter
        }
        var changed = false
        if stale(instruments.windLastSeen) {
            if !instruments.awa.isNaN { instruments.awa = .nan; changed = true }
            if !instruments.aws.isNaN { instruments.aws = .nan; changed = true }
            instruments.windLastSeen = nil
        }
        if stale(instruments.stwLastSeen) {
            if !instruments.stw.isNaN { instruments.stw = .nan; changed = true }
            instruments.stwLastSeen = nil
        }
        if stale(instruments.hdgLastSeen) {
            if !instruments.headingMagDeg.isNaN {
                instruments.headingMagDeg = .nan; changed = true
            }
            instruments.hdgLastSeen = nil
        }
        if stale(instruments.depthLastSeen) {
            if !instruments.depthM.isNaN { instruments.depthM = .nan; changed = true }
            instruments.depthLastSeen = nil
        }
        if stale(instruments.gpsLastSeen) {
            if !instruments.lat.isNaN { instruments.lat = .nan; changed = true }
            if !instruments.lon.isNaN { instruments.lon = .nan; changed = true }
            instruments.gpsLastSeen = nil
        }
        if stale(instruments.seaTempLastSeen) {
            if !instruments.waterTempC.isNaN {
                instruments.waterTempC = .nan; changed = true
            }
            instruments.seaTempLastSeen = nil
        }
        if stale(instruments.airTempLastSeen) {
            if !instruments.airTempC.isNaN {
                instruments.airTempC = .nan; changed = true
            }
            instruments.airTempLastSeen = nil
        }
        // Recompute TWA/TWS/VMG since any of wind/stw going stale
        // invalidates them too.
        if changed { recomputeDerived() }
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
