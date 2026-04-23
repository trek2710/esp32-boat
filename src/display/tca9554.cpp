// tca9554.cpp — see tca9554.h for why this replaces ch422g.cpp.

#include "tca9554.h"

#include <Arduino.h>
#include <Wire.h>

#include "display_pins.h"

namespace display {

// Small helper: write one register byte. Returns the Wire.endTransmission
// error code (0 = ACK'd). Kept as a free function so the compiler can inline
// it and the three caller sites (begin CONFIG, begin OUTPUT, writeOutput) all
// look the same.
static uint8_t writeReg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(TCA9554_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission();
}

bool Tca9554::begin() {
    // Step 1: configure all 8 IO lines as outputs.
    //   CONFIG register bit = 1 → input, bit = 0 → output.
    //   We write 0x00 so every IO drives the output register's bit.
    //
    // This is the moral equivalent of the CH422G "mode" write — without it,
    // whatever we shove into OUTPUT is invisible on the pins.
    uint8_t err = writeReg(TCA9554_REG_CONFIG, 0x00);
    if (err != 0) {
        log_e("[tca9554] CONFIG write to 0x%02X reg 0x%02X NACKed (err=%u)",
              TCA9554_I2C_ADDR, TCA9554_REG_CONFIG, err);
        return false;
    }

    // Step 2: prime the OUTPUT register to the known-safe power-up state.
    // Bit assignments (see display_pins.h — this is the round-13 corrected
    // mapping, sourced from LovyanGFX discussion #630 for this exact board):
    //   bit 0 = LCD_BL    = 0 → backlight off
    //   bit 1 = LCD_RST   = 0 → panel in reset (active low)
    //   bit 2 = TP_RST    = 0 → touch in reset (active low)
    //   bit 3 = LCD_CS    = 0 → panel 3-wire-SPI CS held low (not-selected
    //                           when the bus isn't clocking; driven high/low
    //                           in software for each init byte by the panel
    //                           driver in round 14+)
    //   bits 4..6 unused on this board rev.
    //   bit 7 = BUZZER    = 0 → piezo quiet (active high). Keep OFF so we
    //                           don't chirp during every warm reboot.
    //
    // The panel driver calls resetPanel() + setBits(LCD_BL) later to drive
    // through the init sequence.
    shadow_ = 0;
    if (!writeOutput(shadow_)) {
        log_e("[tca9554] OUTPUT prime-write to 0x%02X reg 0x%02X failed",
              TCA9554_I2C_ADDR, TCA9554_REG_OUTPUT);
        return false;
    }

    log_i("[tca9554] ok (addr=0x%02X, config=0x00, output=0x00)",
          TCA9554_I2C_ADDR);
    return true;
}

bool Tca9554::writeOutput(uint8_t bits) {
    const uint8_t err = writeReg(TCA9554_REG_OUTPUT, bits);
    if (err == 0) {
        shadow_ = bits;
        return true;
    }
    return false;
}

bool Tca9554::setBits(uint8_t mask) {
    return writeOutput(static_cast<uint8_t>(shadow_ | mask));
}

bool Tca9554::clearBits(uint8_t mask) {
    return writeOutput(static_cast<uint8_t>(shadow_ & ~mask));
}

}  // namespace display
