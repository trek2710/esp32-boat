#pragma once

#include <cstdint>

// TCA9554 I2C GPIO expander (@ 0x20) on the ESP32-S3-Touch-AMOLED-1.75-G.
// Several peripheral RESET lines hang off it rather than off ESP32 GPIOs —
// CST9217 touch on EXIO6, LC76G GPS on EXIO7. Requires Wire already begun
// (amoled::begin() does that).

namespace tca9554 {

// Drive EXIO<pin> as a push-pull output at the given level. Read-modify-
// write so other expander pins (audio codec, panel, etc.) stay put.
// Returns false on any I2C error.
bool pinWrite(uint8_t pin, bool high);

}  // namespace tca9554
