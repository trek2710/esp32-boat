// Networking/BusListener.swift — UDP receive on Bus.busPort.
//
// The AP fans each data PGN out to every known STA IP. Once
// HeartbeatPublisher has told the AP our IP, packets land here. Each
// one is a single self-contained JSON document; we extract pgn +
// fields, then route into the BusState callback.

import Foundation
import Network

// NOT @MainActor — NWListener / NWConnection callbacks fire from
// network framework's own dispatch queues. Swift 6's actor-isolation
// checker rejects calls back into MainActor-isolated state from those
// callbacks unless we explicitly bounce. We keep the listener itself
// actor-free and Task-bounce to MainActor only at the final
// `onPacket` callback.
final class BusListener {
    typealias PacketHandler = @MainActor @Sendable (Int, String) -> Void

    private var listener: NWListener?
    private let onPacket: PacketHandler
    private let queue = DispatchQueue(label: "BusListener.udp")
    private var totalReceived: Int = 0

    init(onPacket: @escaping PacketHandler) {
        self.onPacket = onPacket
    }

    func start() throws {
        stop()
        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true
        let port = NWEndpoint.Port(rawValue: Bus.busPort)!
        let l = try NWListener(using: params, on: port)
        l.stateUpdateHandler = { state in
            switch state {
            case .ready:           print("[bl] listener ready on :60001 ✓")
            case .failed(let e):   print("[bl] listener failed: \(e)")
            case .waiting(let e):  print("[bl] listener waiting: \(e)")
            case .cancelled:       print("[bl] listener cancelled")
            default:               break
            }
        }
        l.newConnectionHandler = { [weak self] conn in
            print("[bl] new inbound flow from \(conn.endpoint)")
            self?.handle(conn)
        }
        l.start(queue: queue)
        listener = l
    }

    func stop() {
        listener?.cancel(); listener = nil
    }

    // Each unique remote endpoint (in practice, just the AP) shows up as
    // one NWConnection. We start it, then loop on receiveMessage until
    // it's cancelled.
    private func handle(_ conn: NWConnection) {
        conn.stateUpdateHandler = { state in
            switch state {
            case .failed, .cancelled:
                conn.cancel()
            default:
                break
            }
        }
        conn.start(queue: queue)
        receive(conn)
    }

    private func receive(_ conn: NWConnection) {
        conn.receiveMessage { [weak self] data, _, _, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.totalReceived += 1
                if self.totalReceived <= 3,
                   let s = String(data: data, encoding: .utf8) {
                    print("[bl] rx #\(self.totalReceived) (\(data.count) B): "
                          + s.prefix(120))
                }
                if let s = String(data: data, encoding: .utf8),
                   let pgn = Self.parsePgn(s) {
                    // Log every AIS PGN we receive so we can confirm
                    // the receive path, regardless of whether dispatch
                    // succeeds downstream.
                    if pgn == 129038 || pgn == 129039 || pgn == 129040
                       || pgn == 129809 || pgn == 129810 {
                        print("[bl] AIS pgn=\(pgn): "
                              + s.prefix(160))
                    }
                    let handler = self.onPacket
                    Task { @MainActor in handler(pgn, s) }
                }
            }
            if error != nil {
                print("[bl] conn rx error: \(error!)")
                conn.cancel()
                return
            }
            // NWConnection.receiveMessage is one-shot — re-arm for the
            // next datagram.
            self.receive(conn)
        }
    }

    /// Minimal scanner — find `"pgn":<int>` anywhere in the doc.
    /// We don't full-parse the JSON for this — the firmware doesn't
    /// either (see VirtualBusJson.h), and 99 % of packets are <300 B.
    static func parsePgn(_ s: String) -> Int? {
        guard let range = s.range(of: "\"pgn\":") else { return nil }
        var i = range.upperBound
        // Skip whitespace.
        while i < s.endIndex, s[i] == " " { i = s.index(after: i) }
        // Read digits.
        var value = 0
        var any = false
        while i < s.endIndex, let d = s[i].wholeNumberValue, d >= 0, d <= 9 {
            value = value * 10 + d
            any = true
            i = s.index(after: i)
        }
        return any ? value : nil
    }
}
