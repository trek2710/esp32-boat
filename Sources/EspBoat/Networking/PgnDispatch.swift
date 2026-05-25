// Networking/PgnDispatch.swift — turn an incoming raw JSON document into
// updates against the in-memory state. Mirrors src/NmeaBridge.cpp's
// dispatch() and src_tx/WifiPublisher.cpp's dispatchData() so the
// behaviour stays consistent across peers.
//
// We hand-roll regex-light parsing because the wire schema is fixed
// and tiny (no nested arrays, no escapes in string values). Same
// approach as the firmware's VirtualBusJson.h.

import Foundation

enum PgnDispatch {
    /// Mutates the supplied bundles in place. Returns true if anything
    /// changed (caller can decide whether to repaint).
    static func handle(pgn: Int, json: String,
                       instruments: inout Instruments,
                       targets: inout [AisTarget],
                       peers: inout [String],
                       settings: inout SettingsSnapshot) -> Bool {
        switch pgn {
        case Bus.controlPgn:
            return handleHeartbeat(json: json, peers: &peers, settings: &settings)
        case 130306:
            return handleWind(json: json, instruments: &instruments)
        case 129025:
            return handleGps(json: json, instruments: &instruments)
        case 127250:
            return handleHeading(json: json, instruments: &instruments)
        case 128259:
            return handleStw(json: json, instruments: &instruments)
        case 128267:
            return handleDepth(json: json, instruments: &instruments)
        case 130316:
            return handleTemp(json: json, instruments: &instruments)
        case 129038, 129039, 129040:
            return handleAisPosition(pgn: pgn, json: json, targets: &targets)
        case 129809:
            return handleAisName(json: json, targets: &targets)
        case 129810:
            return handleAisType(json: json, targets: &targets)
        default:
            return false
        }
    }

    // MARK: - heartbeat
    private static func handleHeartbeat(json: String,
                                        peers: inout [String],
                                        settings: inout SettingsSnapshot) -> Bool {
        var changed = false
        if let peer = jsonString(json, key: "peer"),
           !peer.isEmpty,
           !peers.contains(peer),
           peer != Bus.peerName {
            peers.append(peer); changed = true
        }
        // Settings snapshot is only present on AP heartbeats. The
        // wrapper is `settings_v` + a `settings` sub-object.
        if let v = jsonInt(json, key: "settings_v"), v > settings.settingsV {
            // Decode the inner object by giving the JSON decoder a
            // SettingsSnapshot-shaped wrapper.
            if let data = json.data(using: .utf8) {
                struct Envelope: Decodable {
                    let settings_v: Int
                    let settings: BoatSettings?
                }
                if let env = try? JSONDecoder().decode(Envelope.self, from: data),
                   let s = env.settings {
                    settings = SettingsSnapshot(settingsV: env.settings_v,
                                                settings: s)
                    changed = true
                }
            }
        }
        return changed
    }

    // MARK: - data PGNs
    private static func handleWind(json: String,
                                   instruments: inout Instruments) -> Bool {
        guard jsonString(json, key: "reference") == "Apparent",
              let ws = jsonDouble(json, key: "windSpeed"),
              let wa = jsonDouble(json, key: "windAngle") else { return false }
        instruments.awa = wa * 180.0 / .pi
        instruments.aws = ws * 1.943844
        return true
    }

    private static func handleGps(json: String,
                                  instruments: inout Instruments) -> Bool {
        guard let lat = jsonDouble(json, key: "latitude"),
              let lon = jsonDouble(json, key: "longitude") else { return false }
        instruments.lat = lat
        instruments.lon = lon
        return true
    }

    private static func handleHeading(json: String,
                                      instruments: inout Instruments) -> Bool {
        guard let h = jsonDouble(json, key: "heading") else { return false }
        // Accept Magnetic only (matches RX dispatch).
        if let ref = jsonString(json, key: "reference"), ref != "Magnetic" {
            return false
        }
        instruments.headingMagDeg = h * 180.0 / .pi
        return true
    }

    private static func handleStw(json: String,
                                  instruments: inout Instruments) -> Bool {
        guard let s = jsonDouble(json, key: "speedWaterReferenced") else { return false }
        instruments.stw = s * 1.943844
        return true
    }

    private static func handleDepth(json: String,
                                    instruments: inout Instruments) -> Bool {
        guard let d = jsonDouble(json, key: "depth") else { return false }
        instruments.depthM = d
        return true
    }

    private static func handleTemp(json: String,
                                   instruments: inout Instruments) -> Bool {
        guard let src = jsonString(json, key: "source"),
              let tK = jsonDouble(json, key: "actualTemperature") else { return false }
        let tC = tK - 273.15
        switch src {
        case "Sea Temperature":     instruments.waterTempC = tC; return true
        case "Outside Temperature": instruments.airTempC = tC;   return true
        default: return false
        }
    }

    // MARK: - AIS
    private static func handleAisPosition(pgn: Int, json: String,
                                          targets: inout [AisTarget]) -> Bool {
        guard let mmsi = jsonInt(json, key: "userId"), mmsi > 0 else {
            print("[disp] AIS pos pgn=\(pgn): userId parse failed")
            return false
        }
        var t = lookupOrCreate(UInt32(mmsi), in: &targets)
        if let lat = jsonDouble(json, key: "latitude")  { t.lat = lat }
        if let lon = jsonDouble(json, key: "longitude") { t.lon = lon }
        if let s = jsonDouble(json, key: "sog")         { t.sog = s * 1.943844 }
        if let c = jsonDouble(json, key: "cog")         { t.cog = c * 180.0 / .pi }
        if pgn == 129040, let n = jsonString(json, key: "name") { t.name = n }
        t.lastSeen = Date()
        store(t, into: &targets)
        print("[disp] AIS pos mmsi=\(mmsi) → targets.count=\(targets.count)")
        return true
    }

    private static func handleAisName(json: String,
                                      targets: inout [AisTarget]) -> Bool {
        guard let mmsi = jsonInt(json, key: "userId"), mmsi > 0,
              let name = jsonString(json, key: "name") else { return false }
        var t = lookupOrCreate(UInt32(mmsi), in: &targets)
        t.name = name
        t.lastSeen = Date()
        store(t, into: &targets)
        return true
    }

    private static func handleAisType(json: String,
                                      targets: inout [AisTarget]) -> Bool {
        guard let mmsi = jsonInt(json, key: "userId"), mmsi > 0,
              let st = jsonInt(json, key: "typeOfShip") else { return false }
        var t = lookupOrCreate(UInt32(mmsi), in: &targets)
        t.shipType = UInt8(st)
        t.lastSeen = Date()
        store(t, into: &targets)
        return true
    }

    private static func lookupOrCreate(_ mmsi: UInt32,
                                       in targets: inout [AisTarget]) -> AisTarget {
        if let i = targets.firstIndex(where: { $0.mmsi == mmsi }) {
            return targets[i]
        }
        return AisTarget(mmsi: mmsi, name: "")
    }

    private static func store(_ t: AisTarget, into targets: inout [AisTarget]) {
        if let i = targets.firstIndex(where: { $0.mmsi == t.mmsi }) {
            targets[i] = t
        } else {
            targets.append(t)
        }
    }
}

// MARK: - tiny JSON field scanners
private func jsonString(_ s: String, key: String) -> String? {
    let needle = "\"\(key)\":\""
    guard let r = s.range(of: needle) else { return nil }
    let after = s[r.upperBound...]
    guard let end = after.firstIndex(of: "\"") else { return nil }
    return String(after[..<end])
}

private func jsonInt(_ s: String, key: String) -> Int? {
    guard let v = jsonNumberLiteral(s, key: key) else { return nil }
    return Int(v)
}

private func jsonDouble(_ s: String, key: String) -> Double? {
    guard let v = jsonNumberLiteral(s, key: key) else { return nil }
    return Double(v)
}

private func jsonNumberLiteral(_ s: String, key: String) -> Substring? {
    let needle = "\"\(key)\":"
    guard let r = s.range(of: needle) else { return nil }
    var i = r.upperBound
    while i < s.endIndex, s[i] == " " { i = s.index(after: i) }
    let start = i
    while i < s.endIndex {
        let c = s[i]
        if c == "-" || c == "+" || c == "." || c == "e" || c == "E"
            || (c >= "0" && c <= "9") {
            i = s.index(after: i)
        } else { break }
    }
    return start == i ? nil : s[start..<i]
}
