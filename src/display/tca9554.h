// tca9554.h — minimal Waveshare-flavoured TCA9554 I/O expander driver.
//
// Round-9 I2C scan confirmed this board hosts a TCA9554 (not a CH422G, as
// we had wrongly assumed for the first nine rounds). See display_pins.h
// for the full chipset pivot write-up.
//
// The Waveshare ESP32-S3-Touch-LCD-2.1 (ST7701 variant) routes LCD reset,
// touch reset and backlight-enable through this expander on the shared
// I2C bus instead of straight out of the ESP32's GPIO matrix — so no
// pixels and no touch until we've talked to it.
//
// Protocol (from TI's TCA9554 datasheet):
//   Single I2C address 0x20 (A0=A1=A2=GND on this board).
//   Register-pointer protocol: write 1 byte reg addr, then 1 byte data.
//   Registers:
//     0x00 INPUT   — read-only, current state of each IO pin.
//     0x01 OUTPUT  — what each IO drives when configured as an output.
//     0x02 POLINV  — input polarity inversion (we leave at 0).
//     0x03 CONFIG  — per-bit direction. 1 = input, 0 = output. We set
//                    this to 0x00 so all 8 IOs drive outputs.
//
// Compared to the CH422G driver this replaces: the CH422G used a
// multi-address protocol (0x24 for mode, 0x38 for output); TCA9554 uses
// a single address with a register pointer. Otherwise the public API
// (begin / writeOutput / setBits / clearBits / shadow) is intentionally
// identical so the panel driver can swap in without ceremony.
//
// Usage:
//     Tca9554 io;
//     if (!io.begin()) { /* I2C didn't ACK — expander not present */ }
//     io.writeOutput(display::TCA9554_BIT_LCD_BL |
//                    display::TCA9554_BIT_LCD_RST);
//
// The caller owns the reset timing per the panel's power-up sequence.

#pragma once

#include <cstdint>

namespace display {

class Tca9554 {
public:
    // Assumes Wire has already been begun with the right SDA/SCL pins.
    // Writes the CONFIG register to make every IO an output, then primes
    // OUTPUT to 0x00 (LCD_BL off, both resets asserted).
    // Returns true only if every I2C write was ACK'd.
    bool begin();

    // Overwrite the output register with `bits` (IO0..IO7 in that order).
    // Returns true if the I2C write completed without NACK.
    bool writeOutput(uint8_t bits);

    // Convenience — set/clear individual bits without stomping the others.
    // Keeps a software shadow of the last value we wrote so we never need
    // to round-trip a read (the INPUT register on an all-output expander
    // reports the output levels, not what we intended).
    bool setBits(uint8_t mask);
    bool clearBits(uint8_t mask);

    uint8_t shadow() const { return shadow_; }

private:
    uint8_t shadow_ = 0;
};

}  // namespace display
