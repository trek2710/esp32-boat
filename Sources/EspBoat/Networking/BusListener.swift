// Networking/BusListener.swift — UDP receive on Bus.busPort.
//
// The AP fans each data PGN out to every known STA IP. Once
// HeartbeatPublisher has told the AP our IP, packets land here. Each
// one is a single self-contained JSON document; we extract pgn +
// fields, then route into the BusState callback.

import Foundation
import Network

@MainActor
final class BusListener {
    typealias PacketHandler = (Int, String) -> Void  // (pgn, raw JSON)

    private var listener: NWListener?
    private let onPacket: PacketHandler

    init(onPacket: @escaping PacketHandler) {
        self.onPacket = onPacket
    }

    func start() throws {
        stop()
        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true
        let port = NWEndpoint.Port(rawValue: Bus.busPort)!
        let l = try NWListener(using: params, on: port)
        l.newConnectionHandler = { [weak self] conn in
            self?.handle(conn)
        }
        l.start(queue: .main)
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
        conn.start(queue: .main)
        receive(conn)
    }

    private func receive(_ conn: NWConnection) {
        conn.receiveMessage { [weak self] data, _, _, error in
            if let data = data, !data.isEmpty,
               let s = String(data: data, encoding: .utf8) {
                // Single-pass extract of the "pgn" field. Cheap because
                // the field is always near the front of the document.
                if let pgn = Self.parsePgn(s) {
                    self?.onPacket(pgn, s)
                }
            }
            if error != nil {
                conn.cancel()
                return
            }
            // NWConnection.receiveMessage is one-shot — re-arm for the
            // next datagram.
            self?.receive(conn)
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
