#pragma once

#include <stdint.h>

// Reads the pre-baked CM93 chart tiles (.c93t, produced offline by
// tools/chart-transcode) off the microSD and exposes the own-ship cell's
// vector features for the radar to draw underneath the AIS plot. No chart
// decoding happens on the device — just load + iterate.

namespace chart {

// Load the tile for the cell containing (lat,lon) if it isn't already loaded.
// Returns true when a tile is loaded and ready to iterate.
bool ensureCell(double lat, double lon);

bool valid();

// One chart feature. pts points into the loaded tile buffer: npts pairs of
// (lat, lon) float32. layer is a CHART_* id; flags bit0 = closed area.
struct Feature {
    uint8_t       layer;
    uint8_t       flags;
    float         depth;    // DRVAL1/VALDCO metres; NaN if n/a
    const float*  pts;
    uint16_t      npts;
};

void rewind();
bool next(Feature& f);

}  // namespace chart
