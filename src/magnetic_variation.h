// magnetic_variation.h — look up magnetic variation (declination) for a
// given GPS position.
//
// Variation is the angle between True North and Magnetic North at a point on
// the Earth's surface. It changes slowly with location AND time (the magnetic
// pole drifts), so a real implementation reads the current World Magnetic
// Model (WMM) coefficients and evaluates them for (lat, lon, year). The
// device computes its true heading as: heading_true = heading_magnetic + var.
//
// PRESENT (round 53): stub. Returns a hardcoded constant suitable for the
// user's Copenhagen-area cruising ground (≈ +5° east in the late 2020s).
//
// TODO(future): load the WMM coefficient grid from /var/wmm.dat on the SD
// card, evaluate at (lat, lon) for the current epoch (with secular drift
// correction). Cache the most recent lookup so we're not re-evaluating on
// every 10 Hz GPS update — only when the boat moves more than ~10 km.

#pragma once

namespace navmath {

// Returns the magnetic variation at (lat, lon), in degrees.
// Convention: positive = east of true north (i.e. compass needle points
// to the east of true north → true_heading = magnetic_heading + variation).
// Returns NAN if no value is available (lat/lon NaN, or coverage gap).
double lookupMagneticVariation(double lat, double lon);

}  // namespace navmath
