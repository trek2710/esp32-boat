// Networking/BusState.swift — the long-lived observable the SwiftUI
// views read from. Owns the UDP receive listener, the heartbeat
// publisher (PGN 65500 every 5 s while in foreground), and the
// settings-snapshot mirror. Bumped to @MainActor so observers don't
// need to bounce dispatch to update labels.
//
// v1 placeholders: UDP wire-up is stubbed until the JSON dispatcher
// lands (next iteration). HTTP settings already work — try the
// Settings tab against http://192.168.4.1/settings on a paired AP.

import Foundation
import Combine
import Network

@MainActor
final class BusState: ObservableObject {
    // Live published state — Boat / AIS / Diagnostics tabs observe.
    @Published var instruments: Instruments = Instruments()
    @Published var targets: [AisTarget]     = []
    @Published var snapshot: SettingsSnapshot = SettingsSnapshot(
        settingsV: 0, settings: BoatSettings())
    @Published var isOnline: Bool = false
    @Published var lastError: String? = nil

    // Last-known AP peer name + RSSI + peer table snapshot — populated
    // by the heartbeat parser. Empty until first heartbeat.
    @Published var apPeer: String = ""
    @Published var peerNames: [String] = []

    private let settingsClient = SettingsClient()
    private var heartbeatTimer: AnyCancellable?

    // MARK: lifecycle
    func start() {
        Task { await refreshSettings() }
        startHeartbeat()
        // TODO(v1.6 step 2): bring up UDP receive socket on Bus.busPort
        // and wire incoming heartbeats + data PGNs into this state.
        isOnline = true
    }

    func stop() {
        heartbeatTimer?.cancel()
        heartbeatTimer = nil
        isOnline = false
    }

    // MARK: settings
    func refreshSettings() async {
        do {
            let s = try await settingsClient.fetch()
            self.snapshot = s
            self.lastError = nil
        } catch {
            self.lastError = error.localizedDescription
        }
    }

    /// Patch one key. The UI calls this from a Toggle / Stepper /
    /// Slider on commit; optimistic update happens after the AP echoes
    /// back so we converge on the authoritative version number.
    func apply<E: Encodable>(_ key: String, _ value: E) async {
        do {
            let s = try await settingsClient.apply([key: AnyEncodable(value)])
            self.snapshot = s
            self.lastError = nil
        } catch {
            self.lastError = error.localizedDescription
        }
    }

    // MARK: heartbeat — placeholder
    // The full heartbeat publisher uses NWConnection with .udp on
    // Bus.apHost / Bus.busPort, and writes the same JSON shape
    // RoleNegotiator::buildHeartbeatJson emits on the firmware side.
    // For v1 step 2 we'll fill this in; for now the timer just fires
    // so the cadence wiring is in place.
    private func startHeartbeat() {
        heartbeatTimer = Timer.publish(every: Bus.heartbeatPeriod,
                                       on: .main, in: .common)
            .autoconnect()
            .sink { [weak self] _ in
                // self?.sendHeartbeat()    — wired in next iteration
                _ = self
            }
    }
}
