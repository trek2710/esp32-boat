// cst820.h — CST820 capacitive touch controller driver.
//
// On the Waveshare ESP32-S3-Touch-LCD-2.1 (ST7701 RGB variant) the CST820
// hangs off the same I2C bus as the TCA9554 expander and PCF85063 RTC at
// I2C address 0x15. Reset is NOT a real GPIO — it's TCA9554 IO1 / EXIO2
// (active-low). The TP_INT line is a real GPIO (GPIO4) that the chip
// pulls low to flag a touch event; we don't currently use it as an
// interrupt, we just poll the touch register on each LVGL input read.
//
// Register layout (from the CST820 register map distributed with Waveshare's
// touch demo for this board family — also matches the Hynitron CST816S which
// shares its protocol). Note the +1 offset compared to the CST816S "register
// pointer starts at 0" docs floating around online: the CST820 puts gesture
// at reg 0x01, not 0x00. Round 36 mis-read this and round 37 corrected it.
//
//   0x01  Gesture (1=swipe up, 2=swipe down, 3=swipe left, 4=swipe right,
//                  5=single tap, 11=double tap, 12=long press)
//   0x02  Number of fingers detected (0 or 1)
//   0x03  X position high byte (top 4 bits in low nibble; event flag in
//         the high nibble)
//   0x04  X position low  byte
//   0x05  Y position high byte (top 4 bits in low nibble)
//   0x06  Y position low  byte
//
// X and Y are 12-bit values; the high nibbles of bytes 0x03/0x05 hold the
// upper 4 bits of the 12-bit coordinate. read() requests 6 bytes starting
// at register 0x01.
//
// Coordinate system: native panel orientation is X=0..479, Y=0..479. We
// rotate 180° in flushCb to fix the upside-down panel, so callers want
// (DISPLAY_WIDTH-1-x, DISPLAY_HEIGHT-1-y). That rotation lives in the LVGL
// indev read_cb in Ui.cpp — this driver returns raw panel coordinates.
//
// Usage:
//   display::Cst820 touch;
//   if (!touch.begin(g_expander)) { /* probe failed */ }
//   uint16_t x, y;
//   if (touch.read(&x, &y)) { /* finger present at (x, y) */ }
//
// Reset sequence (from the CST820 datasheet):
//   1. Drive TP_RST low for ≥10 ms.
//   2. Release TP_RST high.
//   3. Wait ≥50 ms for the chip to boot before issuing I2C commands.

#pragma once

#include <cstdint>

namespace display {

class Tca9554;  // forward declaration so we don't pull in the whole header

class Cst820 {
public:
    // Pulses TP_RST via the expander, gives the chip 50 ms to boot, then
    // probes I2C 0x15 for an ACK. Returns true only if the probe ACKed —
    // meaning the controller is responding and ready for register reads.
    // Caller must have already initialised Wire and the TCA9554 expander.
    bool begin(Tca9554& expander);

    // Read one finger position. Returns true if a finger is currently
    // touching the panel and writes the raw (pre-rotation) coordinates to
    // *x and *y. Returns false if no finger is present, the I2C read NACKs,
    // or begin() never ran. Cheap enough (8 bytes over 100 kHz I2C ≈ 800 µs)
    // to call from LVGL's input-device read_cb every input poll.
    bool read(uint16_t* x, uint16_t* y);

    // True after a successful begin().
    bool ready() const { return ready_; }

private:
    bool ready_ = false;
};

}  // namespace display
