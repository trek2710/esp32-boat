// Models/Settings.swift — Swift mirror of include/Settings.h on the firmware.
//
// Keys + defaults track ADR-0013 v1. The decoder accepts either of the
// two wire shapes that ship from the firmware:
//
//   * The HTTP GET /settings response:
//       { "settings_v": N, "settings": { "sim.wind": true, ... } }
//   * The heartbeat field embed (same shape, but the AP wraps it inside
//     the PGN 65500 "fields" object — handled by HeartbeatParser).

import Foundation

struct BoatSettings: Codable, Equatable {
    var simWind: Bool         = true
    var simGps: Bool          = true
    var simHeading: Bool      = true
    var simDepth: Bool        = true
    var simSeaTemp: Bool      = true
    var simAirTemp: Bool      = true

    var navNoGoDeg: Int       = 35

    var aisRangeNm: Int       = 12
    var aisHideAnchored: Bool = true
    var aisStaleS: Int        = 600

    var uiBrightness: Int     = 80
    var uiIdleDimAfterS: Int  = 300

    enum CodingKeys: String, CodingKey {
        case simWind         = "sim.wind"
        case simGps          = "sim.gps"
        case simHeading      = "sim.heading"
        case simDepth        = "sim.depth"
        case simSeaTemp      = "sim.sea_temp"
        case simAirTemp      = "sim.air_temp"
        case navNoGoDeg      = "nav.no_go_deg"
        case aisRangeNm      = "ais.range_nm"
        case aisHideAnchored = "ais.hide_anchored"
        case aisStaleS       = "ais.stale_s"
        case uiBrightness    = "ui.brightness"
        case uiIdleDimAfterS = "ui.idle_dim_after_s"
    }
}

/// Top-level wrapper used by both the HTTP endpoint and the heartbeat
/// embed: `{ "settings_v": N, "settings": { ... } }`.
struct SettingsSnapshot: Codable, Equatable {
    let settingsV: Int
    let settings: BoatSettings

    enum CodingKeys: String, CodingKey {
        case settingsV = "settings_v"
        case settings
    }
}
