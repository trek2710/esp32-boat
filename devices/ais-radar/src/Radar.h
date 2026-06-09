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

// Worst threat across all targets: 0=none, 1=safe, 2=alert, 3=danger. Used
// for the background tint and shared with the iOS app over BLE.
int assessWorst(AisTargetStore& store, double ownLat, double ownLon,
                double ownCogDeg, double ownSogKn);

// Redraw. ownLat/ownLon centre the plot; ownCogDeg orients the own-ship icon
// (NaN → pointing up); ownSogKn (≤0 = stationary) feeds the collision test.
// The background is tinted by the worst target threat (green/amber/red).
void draw(AisTargetStore& store, double ownLat, double ownLon,
          double ownCogDeg, double ownSogKn, int batteryPct);

}  // namespace radar
