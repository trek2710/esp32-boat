// Ui — minimal LVGL instrument dashboard.
//
// v1 draws three pages, cycled by swipe-left / swipe-right:
//   0. Overview — classic-boating wind compass (needle = AWA, centre = AWS),
//                 big SOG readout, small HDG/COG underneath.
//   1. Data     — every value from BoatState laid out in a compact grid.
//   2. Debug    — rolling log of received NMEA 2000 PGNs (PGN / age / summary).
//
// This header is intentionally minimal; screen construction happens in Ui.cpp.
// When you want to customise appearance, start there.

#pragma once

#include "BoatState.h"

namespace ui {

// Initialise LVGL, the display driver, the touch driver, and build screens.
// Must be called from the task that will own the UI loop (typically loop() on
// core 1 — the Arduino default).
void begin(BoatState& state);

// Pump LVGL timers + refresh visible values from BoatState. Call often (e.g.
// every loop() iteration). Returns the number of ms until the next LVGL timer
// wants to run, so the caller can delay appropriately.
uint32_t tick();

}  // namespace ui
