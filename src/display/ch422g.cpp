// ch422g.cpp — see ch422g.h for why this exists.

#include "ch422g.h"

#include <Arduino.h>
#include <Wire.h>

#include "display_pins.h"

namespace display {

bool Ch422g::begin() {
    // Probe the expander by starting an I2C transmission to its output-register
    // address and checking for an ACK. No register byte is written because the
    // CH422G's output "register" *is* whatever byte follows the address.
    Wire.beginTransmission(CH422G_I2C_ADDR_OUTPUT);
    const uint8_t err = Wire.endTransmission();
    if (err != 0) {
        // err 2 = NACK on address, err 5 = timeout. Either way the expander
        // is not responding. Caller will log + fall back.
        return false;
    }
    // Start with everything held: backlight off, both resets asserted (reset
    // lines are active low, so a '0' bit means "in reset"). After this the
    // caller sequences the panel bring-up.
    shadow_ = 0;
    return writeOutput(shadow_);
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
