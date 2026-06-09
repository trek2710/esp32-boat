#pragma once

#include <cstdint>

// AMOLED display bring-up for the Waveshare ESP32-S3-Touch-AMOLED-1.75-G.
//
// Salvaged from the v1 TX firmware (src_tx/main.cpp), the hard-won bit:
// SH8601/CO5300 AMOLED over QSPI via Arduino_GFX, bound to LVGL 8.3 with a
// full-screen RGB565 PSRAM draw buffer. Brings up I2C + the AXP2101 PMIC
// first (the panel rides its default rails).
//
// After begin() returns true, lv_scr_act() is a ready 466×466 black screen.
// The caller drives LVGL in loop():
//     uint32_t now = millis();
//     lv_tick_inc(now - last); last = now;
//     lv_timer_handler();

namespace amoled {

constexpr int     kI2cSda = 15;
constexpr int     kI2cScl = 14;
constexpr int16_t kLcdW   = 466;
constexpr int16_t kLcdH   = 466;

// Bring up I2C, AXP2101, the AMOLED panel, and LVGL. Returns false (with a
// reason on Serial) if any stage fails.
bool begin();

// Battery charge from the AXP2101 fuel gauge (0–100), or < 0 if unavailable /
// no battery (e.g. running on USB power only).
int batteryPercent();

}  // namespace amoled
