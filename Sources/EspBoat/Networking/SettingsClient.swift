// Networking/SettingsClient.swift — HTTP control plane (ADR-0013).
//
// GET /settings — fetch the AP's canonical snapshot.
// POST /settings — apply a partial key/value JSON, bump version, persist
// to NVS on the AP; the change fans out to every STA on the next
// heartbeat (~5 s).
//
// The AP only listens while it has the AP role. If POST fails with a
// connection error, treat it as "AP handoff in progress" and retry on
// the next user action.

import Foundation

struct SettingsClient {
    let baseURL: URL

    init(host: String = Bus.apHost, port: UInt16 = Bus.httpPort) {
        self.baseURL = URL(string: "http://\(host):\(port)")!
    }

    /// Returns the AP's current snapshot. Throws on transport or decode error.
    func fetch() async throws -> SettingsSnapshot {
        var req = URLRequest(url: baseURL.appendingPathComponent("settings"))
        req.httpMethod = "GET"
        req.timeoutInterval = 4.0
        let (data, response) = try await URLSession.shared.data(for: req)
        try Self.ensureOK(response)
        return try JSONDecoder().decode(SettingsSnapshot.self, from: data)
    }

    /// Apply a partial update. `changes` is a flat dict keyed on the
    /// ADR-0013 keys (`"sim.wind"`, `"nav.no_go_deg"`, etc.). Returns
    /// the post-apply snapshot the AP echoes back.
    @discardableResult
    func apply(_ changes: [String: AnyEncodable]) async throws -> SettingsSnapshot {
        var req = URLRequest(url: baseURL.appendingPathComponent("settings"))
        req.httpMethod = "POST"
        req.timeoutInterval = 4.0
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try JSONEncoder().encode(changes)
        let (data, response) = try await URLSession.shared.data(for: req)
        try Self.ensureOK(response)
        return try JSONDecoder().decode(SettingsSnapshot.self, from: data)
    }

    private static func ensureOK(_ resp: URLResponse) throws {
        guard let http = resp as? HTTPURLResponse else {
            throw SettingsError.transport("non-HTTP response")
        }
        guard (200..<300).contains(http.statusCode) else {
            throw SettingsError.status(http.statusCode)
        }
    }
}

enum SettingsError: LocalizedError {
    case transport(String)
    case status(Int)

    var errorDescription: String? {
        switch self {
        case .transport(let s): return "Transport error: \(s)"
        case .status(let s):    return "HTTP \(s) from AP"
        }
    }
}

/// Tiny shim to let us POST a heterogeneous JSON dict (`sim.wind: true`,
/// `nav.no_go_deg: 40`) without resorting to `Any` or hand-rolled
/// serialisation. Wraps any Encodable; encodes through to its body.
struct AnyEncodable: Encodable {
    private let _encode: (Encoder) throws -> Void
    init<E: Encodable>(_ value: E) {
        self._encode = { try value.encode(to: $0) }
    }
    func encode(to encoder: Encoder) throws { try _encode(encoder) }
}
