#pragma once

#include <cstdint>

// CST9217 capacitive touch on the ESP32-S3-Touch-AMOLED-1.75-G, via
// lewisxhe/SensorLib's TouchDrvCST92xx (the CST9217 uses 16-bit register
// addresses, so generic CST816 drivers don't work). RESET is behind the
// TCA9554 expander (EXIO6). Requires Wire + the expander up
// (amoled::begin()).
//
// The chip reports raw panel coordinates that already match LVGL's
// coordinate system in the firmware's native (USB-right) orientation — no
// transform needed.
//
// NOTE: the device env must run scripts/patch_sensorlib.py (pre-build) to
// silence SensorLib's getPoint(0) log spam — see that script.

namespace touch {

// Reset + bring up the CST9217. Returns false on I2C/driver error.
bool begin();

// If a finger is present this poll, fill x/y (panel pixels) and return true.
bool readPoint(int16_t &x, int16_t &y);

}  // namespace touch
