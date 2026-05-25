// Models/Instruments.swift — local mirror of src/BoatState.h's Instruments.
//
// One value per PGN class. NaN means "never seen / cleared by remote
// staleness signal." The BusListener populates this on every incoming
// data PGN; the Boat tab observes it.

import Foundation

struct Instruments: Equatable {
    // GPS + motion-over-ground
    var lat: Double = .nan
    var lon: Double = .nan
    var sog: Double = .nan      // knots
    var cog: Double = .nan      // degrees true

    // Apparent wind (masthead frame)
    var awa: Double = .nan      // degrees, ± from bow
    var aws: Double = .nan      // knots

    // True wind (derived)
    var twa: Double = .nan
    var tws: Double = .nan
    var twd: Double = .nan

    // Heading + variation
    var headingMagDeg: Double  = .nan
    var headingTrueDeg: Double = .nan
    var magneticVariationDeg: Double = .nan

    // Speed through water + depth + temps
    var stw: Double         = .nan
    var depthM: Double      = .nan
    var waterTempC: Double  = .nan
    var airTempC: Double    = .nan

    // Derived helpers
    var vmg: Double = .nan
}

struct AisTarget: Identifiable, Equatable {
    var mmsi: UInt32
    var name: String
    var lat: Double = .nan
    var lon: Double = .nan
    var sog: Double = .nan
    var cog: Double = .nan
    var shipType: UInt8 = 0
    var lastSeen: Date = Date()

    var id: UInt32 { mmsi }
}
