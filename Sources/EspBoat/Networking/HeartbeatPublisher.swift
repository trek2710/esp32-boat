// Networking/HeartbeatPublisher.swift — sends PGN-65500 heartbeats to
// the AP at 5-second cadence, identifying us as `kind:"ios-app"`,
// priority 80. The AP records the source IP on receive (ADR-0010
// peerSeen()), then starts fanning every data PGN it sees out to us.
// Without this, the AP never learns our IP and the receive listener
// stays silent.

import Foundation
import Network

// NOT @MainActor — DispatchSource timers and NWConnection callbacks
// fire from non-main queues. See BusListener.swift for the same
// reasoning. This class doesn't touch UI state directly, so a non-
// isolated home is fine.
final class HeartbeatPublisher {
    private var connection: NWConnection?
    private var timer: DispatchSourceTimer?
    private let bootMs: UInt64 = UInt64(ProcessInfo.processInfo.systemUptime * 1000)
    private let queue = DispatchQueue(label: "HeartbeatPublisher.udp")

    func start() {
        stop()
        let host = NWEndpoint.Host(Bus.apHost)
        let port = NWEndpoint.Port(rawValue: Bus.busPort)!
        let c = NWConnection(host: host, port: port, using: .udp)
        c.stateUpdateHandler = { state in
            // No-op; UDP connections are stateless from our POV, but the
            // handler is required for the NWConnection to schedule work.
            _ = state
        }
        c.start(queue: queue)
        connection = c

        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 0.5, repeating: Bus.heartbeatPeriod)
        t.setEventHandler { [weak self] in self?.sendOne() }
        t.resume()
        timer = t
    }

    func stop() {
        timer?.cancel(); timer = nil
        connection?.cancel(); connection = nil
    }

    private func sendOne() {
        guard let c = connection else { return }
        let nowMs = UInt64(ProcessInfo.processInfo.systemUptime * 1000) - bootMs
        // Match RoleNegotiator::buildHeartbeatJson's shape exactly so
        // the firmware's findInt/findString parser accepts it. No
        // settings block (only the AP emits that). `ap_ip` is empty for
        // STAs.
        let payload =
            "{\"pgn\":\(Bus.controlPgn)," +
            "\"src\":255," +
            "\"peer\":\"\(Bus.peerName)\"," +
            "\"fields\":{" +
                "\"event\":\"heartbeat\"," +
                "\"kind\":\"\(Bus.kind)\"," +
                "\"priority\":\(Bus.priority)," +
                "\"role\":\"STA\"," +
                "\"ap_ip\":\"\"," +
                "\"uptime_ms\":\(nowMs)" +
            "}}"
        guard let data = payload.data(using: .utf8) else { return }
        c.send(content: data, completion: .contentProcessed { _ in })
    }
}
