#pragma once

#include <AisTargetStore.h>

// North-up PPI radar on the AMOLED: own ship centred, AIS targets at their
// range/bearing from the own position, with 5-min COG projection sticks,
// labels, range rings, and stale-contact dimming. Auto-fits the range to the
// farthest target (floored). Re-targeted from the v1 RX LVGL radar.

namespace radar {

// Allocate the canvas + create it on the active screen. Call once after the
// display is up.
void begin();

// Redraw. ownLat/ownLon centre the plot; ownCogDeg orients the own-ship icon
// (NaN → pointing up).
void draw(AisTargetStore& store, double ownLat, double ownLon, double ownCogDeg);

}  // namespace radar
