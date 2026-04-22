// ch422g.h — minimal Waveshare-flavoured CH422G I/O expander driver.
//
// The Waveshare ESP32-S3-Touch-LCD-2.1 routes the LCD reset, touch reset and
// backlight-enable signals through a CH422G I/O expander on the shared I2C
// bus instead of straight out of the ESP32's GPIO matrix. Nothing in our
// firmware can bring the panel out of reset or turn on the backlight until
// we've talked to this chip.
//
// We only need the OUTPUT port — we drive EXIO0..3 high or low to control
// LCD_BL / TP_RST / LCD_RST / SD_CS (see display_pins.h for bit assignments).
// Inputs, IRQs, and mode-register writes aren't needed for v1.
//
// Usage:
//     Ch422g io;
//     if (!io.begin()) { /* I2C didn't ACK — expander not present */ }
//     io.writeOutput(display::CH422G_BIT_LCD_BL | display::CH422G_BIT_LCD_RST);
//
// The caller is responsible for holding / releasing resets according to the
// panel's power-up sequence (see st77916_panel.cpp).

#pragma once

#include <cstdint>

namespace display {

class Ch422g {
public:
    // Assumes Wire (the Arduino default I2C instance) has already been begun
    // with the right SDA/SCL pins. Returns true if the chip ACK'd on the
    // output address.
    bool begin();

    // Overwrite the output register with `bits` (one byte, EXIO0..EXIO7).
    // Returns true if the I2C write completed without NACK.
    bool writeOutput(uint8_t bits);

    // Convenience — set/clear individual bits without stomping the others.
    // Keeps a software shadow of the last value we wrote, because the chip's
    // output register is not readable in this minimal mode.
    bool setBits(uint8_t mask);
    bool clearBits(uint8_t mask);

    uint8_t shadow() const { return shadow_; }

private:
    uint8_t shadow_ = 0;
};

}  // namespace display
