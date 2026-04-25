// cst820.cpp — see cst820.h for the protocol notes.

#include "cst820.h"

#include <Arduino.h>
#include <Wire.h>

#include "display_pins.h"
#include "tca9554.h"

namespace display {

bool Cst820::begin(Tca9554& expander) {
    // Step 1: pulse the reset line via TCA9554 IO1 (TP_RST, active low).
    //
    // Per the CST820 datasheet the chip needs at least 10 ms held low and
    // ≥50 ms post-release before it'll accept I2C traffic; we use 20/100 to
    // give margin against a noisy 3V3 rail.
    //
    // Note that TP_RST shares a bus (single I2C expander) with LCD_RST,
    // LCD_CS, and BUZZER. setBits/clearBits are read-modify-write against
    // the shadow, so they don't disturb whatever the panel driver has
    // configured for those other bits.
    if (!expander.clearBits(TCA9554_BIT_TP_RST)) {
        log_e("[cst820] could not assert TP_RST low via TCA9554");
        return false;
    }
    delay(20);
    if (!expander.setBits(TCA9554_BIT_TP_RST)) {
        log_e("[cst820] could not release TP_RST high via TCA9554");
        return false;
    }
    delay(100);

    // Step 2: configure the INT pin as input. We don't currently wire it as
    // an interrupt — LVGL's read_cb runs once per LVGL tick (~30 ms) which
    // is well below the 100 Hz CST820 sample rate, so polling is fine.
    pinMode(TP_PIN_INT, INPUT);

    // Step 3: probe by I2C address. Don't try to read a register here — the
    // CST820's auto-sleep wakes on the first I2C transaction, and we want a
    // clean ACK before issuing anything that could be retried oddly.
    Wire.beginTransmission(TP_I2C_ADDR);
    const uint8_t err = Wire.endTransmission();
    if (err != 0) {
        log_e("[cst820] probe at 0x%02X NACKed (err=%u)", TP_I2C_ADDR, err);
        ready_ = false;
        return false;
    }

    log_i("[cst820] ok (addr=0x%02X, INT=GPIO%d)", TP_I2C_ADDR, TP_PIN_INT);
    ready_ = true;
    return true;
}

bool Cst820::read(uint16_t* x, uint16_t* y) {
    if (!ready_ || x == nullptr || y == nullptr) return false;

    // Round 37 fix: start the read at register 0x01, not 0x00.
    //
    // Round 36's first cut read 6 bytes from 0x00, putting the gesture byte
    // (reg 0x01) at buf[0] and finger count (reg 0x02) at buf[1] — except
    // it actually put reg 0x00 at buf[0], gesture at buf[1], and finger
    // count at buf[2]. Result: `if (fingers == 0)` was reading the
    // *gesture* register, which is 0 unless a recognised gesture just
    // fired, so single-finger taps and drags were being silently dropped.
    //
    // Waveshare's CST820 demo for the ESP32-S3-Touch-LCD-2.1 documents the
    // register layout as:
    //   0x01  Gesture
    //   0x02  Number of fingers (0 or 1)
    //   0x03  X high  (top 4 bits in low nibble; event flag in high nibble)
    //   0x04  X low
    //   0x05  Y high  (top 4 bits in low nibble)
    //   0x06  Y low
    //
    // Read 6 bytes starting at 0x01 → [gesture, fingers, xH, xL, yH, yL].
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(0x01));
    if (Wire.endTransmission(false) != 0) return false;  // repeated start

    constexpr size_t kNeeded = 6;
    const size_t got = Wire.requestFrom(static_cast<int>(TP_I2C_ADDR),
                                        static_cast<int>(kNeeded));
    if (got != kNeeded) return false;

    uint8_t buf[kNeeded];
    for (size_t i = 0; i < kNeeded; ++i) buf[i] = Wire.read();

    // buf[0] = gesture (unused for now), buf[1] = finger count.
    const uint8_t fingers = buf[1];
    if (fingers == 0) return false;  // no touch

    // 12-bit X = (buf[2] & 0x0F) << 8 | buf[3]
    // 12-bit Y = (buf[4] & 0x0F) << 8 | buf[5]
    *x = static_cast<uint16_t>((static_cast<uint16_t>(buf[2] & 0x0F) << 8) | buf[3]);
    *y = static_cast<uint16_t>((static_cast<uint16_t>(buf[4] & 0x0F) << 8) | buf[5]);
    return true;
}

}  // namespace display
