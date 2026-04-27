// magnetic_variation.cpp — see magnetic_variation.h for the rationale.

#include "magnetic_variation.h"

#include <cmath>

namespace navmath {

double lookupMagneticVariation(double lat, double lon) {
    // Stub: return a constant suitable for Copenhagen / SW Baltic. Per
    // NOAA's WMM-2025, declination at (55.68 N, 12.57 E) is ≈ +5.0°
    // (east) and changes by < 0.2° per year. Good enough for v1 — the
    // dial only quotes integer degrees and the user's cruising ground
    // is small.
    //
    // TODO: load WMM coefficients from /sd/wmm.dat once the SD-card
    // module lands. Evaluate at (lat, lon) for the current epoch with
    // secular drift. Cache the result; only recompute when the boat
    // has moved more than ~10 km since the previous lookup.
    if (std::isnan(lat) || std::isnan(lon)) {
        return NAN;
    }
    return 5.0;
}

}  // namespace navmath
