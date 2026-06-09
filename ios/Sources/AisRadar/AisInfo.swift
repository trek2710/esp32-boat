import Foundation
import SwiftUI

enum ShipIcon { case hull, circle, triangle }

// AIS shipType → label + radar icon. Mirrors the device's icon choice
// (cargo/tanker = hull, pleasure = circle, sailing/other = triangle).
func shipTypeName(_ t: UInt8) -> String {
    switch Int(t) {
    case 0:        return "—"
    case 30:       return "Fishing"
    case 31, 32:   return "Towing"
    case 36:       return "Sailing"
    case 37:       return "Pleasure"
    case 40...49:  return "High-speed"
    case 50:       return "Pilot"
    case 51:       return "SAR"
    case 52:       return "Tug"
    case 55:       return "Law"
    case 60...69:  return "Passenger"
    case 70...79:  return "Cargo"
    case 80...89:  return "Tanker"
    default:       return "Other"
    }
}

func shipTypeIcon(_ t: UInt8) -> ShipIcon {
    switch Int(t) {
    case 70...89: return .hull
    case 37:      return .circle
    default:      return .triangle
    }
}

struct TargetEval {
    var threat: Int            // 0 none,1 safe,2 alert,3 danger
    var rangeNm: Double
    var bearingDeg: Double
    var cpaNm: Double?
    var tcpaSec: Double?
}

// Client-side threat + CPA/TCPA. The device sends only the worst threat (which
// folds in own motion); here own SOG isn't on the wire, so CPA treats own ship
// as stationary — fine on the bench, approximate under way.
func evaluateTarget(_ t: AisTarget, own: OwnShip) -> TargetEval {
    guard let olat = own.lat, let olon = own.lon else {
        return TargetEval(threat: 1, rangeNm: 0, bearingDeg: 0, cpaNm: nil, tcpaSec: nil)
    }
    let (rng, brg) = rangeBearing(olat, olon, t.lat, t.lon)
    let rangeM = rng * 1852.0
    var threat = 1
    let sog = t.sogKn ?? -1, cog = t.cogDeg ?? -1
    if (sog > 15 && rangeM < 5000) || (sog > 0.2 && rangeM < 200) { threat = 2 }

    var cpaNm: Double?, tcpaSec: Double?
    if sog >= 0 && cog >= 0 {
        let d2r = Double.pi / 180, kn2ms = 0.514444
        let e = (t.lon - olon) * d2r * 6_371_000 * cos(olat * d2r)
        let n = (t.lat - olat) * d2r * 6_371_000
        let tVe = sog * kn2ms * sin(cog * d2r), tVn = sog * kn2ms * cos(cog * d2r)
        let vv = tVe * tVe + tVn * tVn
        if vv > 1e-6 {
            let tcpa = -(e * tVe + n * tVn) / vv
            if tcpa > 0 {
                let ce = e + tVe * tcpa, cn = n + tVn * tcpa
                let cpa = (ce * ce + cn * cn).squareRoot()
                cpaNm = cpa / 1852.0; tcpaSec = tcpa
                if tcpa < 360 && cpa < 185 { threat = 3 }
            }
        }
    }
    return TargetEval(threat: threat, rangeNm: rng, bearingDeg: brg, cpaNm: cpaNm, tcpaSec: tcpaSec)
}

// Threat marker colour (matches the device): safe dark, alert yellow, danger red.
func threatMarkColor(_ level: Int) -> Color {
    switch level {
    case 3:  return Color(red: 0.88, green: 0.0, blue: 0.0)
    case 2:  return Color(red: 0.78, green: 0.78, blue: 0.0)
    default: return Color(red: 0.08, green: 0.13, blue: 0.17)
    }
}
