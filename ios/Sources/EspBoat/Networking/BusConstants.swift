// Networking/BusConstants.swift — wire constants pinned from ADR-0010
// and ADR-0013 on the firmware side. Single source of truth here for
// the iOS app; keep in sync with include/VirtualBusJson.h's kApIp +
// kBusPort and the heartbeat schema.

import Foundation

enum Bus {
    /// AP IP (softAP gateway). All HTTP control + UDP traffic targets this.
    static let apHost = "192.168.4.1"

    /// UDP port for the JSON PGN bus (heartbeats + data).
    static let busPort: UInt16 = 60001

    /// HTTP control plane port.
    static let httpPort: UInt16 = 80

    /// Control PGN (heartbeat). PGN 65500 — manufacturer-proprietary range.
    static let controlPgn: Int = 65500

    /// Heartbeat cadence — match RoleNegotiator::kHeartbeatPeriodMs.
    static let heartbeatPeriod: TimeInterval = 5.0

    /// This peer's role in the bus.
    static let kind: String     = "ios-app"
    static let peerName: String = "ios-app"
    static let priority: Int    = 80
}
