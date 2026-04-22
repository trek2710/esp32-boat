// ch422g.cpp — see ch422g.h for why this exists.

#include "ch422g.h"

#include <Arduino.h>
#include <Wire.h>

#include "display_pins.h"

namespace display {

bool Ch422g::begin() {
    // CH422G uses multiple I2C addresses (one per internal register) instead
    // of the usual <address, register-pointer, data> protocol. We:
    //   1. Write 0x01 to the MODE register (0x24) → "IO0..IO7 are outputs".
    //      Without this the WR_IO register has no effect, and our LCD_RST /
    //      LCD_BL / TP_RST lines stay floating (i.e. panel never comes out
    //      of reset, backlight never turns on).
    //   2. Write 0x00 to the OUTPUT register (0x38) → backlight off, both
    //      resets asserted (active-low, so bit=0 = in-reset). St77916Panel
    //      sequences reset/backlight from here.
    //
    // Bus-level diagnostics (which addresses ACK at all) live in Ui.cpp so
    // they run before we get here and can auto-correct a swapped SDA/SCL.

    // ---- step 1: configure mode register --------------------------------
    Wire.beginTransmission(CH422G_I2C_ADDR_MODE);
    Wire.write(0x01);                    // IO_OE = 1 — set IO0..IO7 as outputs
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        log_e("[ch422g] mode-register write to 0x%02X NACKed (err=%u)",
              CH422G_I2C_ADDR_MODE, err);
        return false;
    }

    // ---- step 2: prime the output register ------------------------------
    shadow_ = 0;
    if (!writeOutput(shadow_)) {
        log_e("[ch422g] output-register write to 0x%02X failed",
              CH422G_I2C_ADDR_OUTPUT);
        return false;
    }
    log_i("[ch422g] ok (mode=0x%02X, output=0x%02X)",
          CH422G_I2C_ADDR_MODE, CH422G_I2C_ADDR_OUTPUT);
    return true;
}

bool Ch422g::writeOutput(uint8_t bits) {
    Wire.beginTransmission(CH422G_I2C_ADDR_OUTPUT);
    Wire.write(bits);
    const uint8_t err = Wire.endTransmission();
    if (err == 0) {
        shadow_ = bits;
        return true;
    }
    return false;
}

bool Ch422g::setBits(uint8_t mask) {
    return writeOutput(static_cast<uint8_t>(shadow_ | mask));
}

bool Ch422g::clearBits(uint8_t mask) {
    return writeOutput(static_cast<uint8_t>(shadow_ & ~mask));
}

}  // namespace display
