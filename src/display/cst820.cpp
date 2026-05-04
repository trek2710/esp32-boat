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

    // Step 4 (round 38): disable auto-sleep.
    //
    // The CST820 (like its CST816S sibling) goes into a low-power sleep state
    // after a few hundred ms of idle. The first I2C read after sleep returns
    // a stale/zero buffer instead of waking the chip up cleanly. Round 37's
    // register-offset fix didn't help because, in normal use, every read
    // happens after a long enough idle that the chip is still asleep when
    // we ask it for a finger count.
    //
    // The fix is the standard CST816S/CST820 incantation: write 0xFF to
    // register 0xFE ("DisableAutoSleep"). Documented in the Hynitron CST816S
    // datasheet's register map (CST820 inherits the same protocol) and in
    // essentially every community driver (fbiego/CST816S, lewisxhe/SensorLib,
    // Bodmer/TFT_eSPI's CST820 demo). Non-fatal if it NACKs — we log a
    // warning and carry on.
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(0xFE));
    Wire.write(static_cast<uint8_t>(0xFF));
    const uint8_t sleep_err = Wire.endTransmission();
    if (sleep_err != 0) {
        log_w("[cst820] DisableAutoSleep write NACKed (err=%u) — touch may "
              "miss the first tap after idle", sleep_err);
    }

    // Step 5 (round 59): enable continuous-slide reporting.
    //
    // The CST816S/CST820 ships with bits 1+2 of MotionMask (register
    // 0xEC) cleared. In that default mode the chip publishes the
    // INITIAL touch point and then goes mostly silent for the rest of
    // the gesture — round-58's monitor trace showed every failed swipe
    // had dx=0 dy=0 because last_x/last_y were never updated past the
    // press point.
    //
    //   0xEC bit 0  EnDClick — enable double-click gesture
    //   0xEC bit 1  EnConUD  — "Slide up and down to enable continuous operation"
    //   0xEC bit 2  EnConLR  — "Continuous operation can slide around"
    //
    // Writing 0x06 (bits 1 + 2) makes the chip push a fresh x/y on
    // every 100 Hz internal sample for the whole duration of a slide,
    // which is what our touchReadCb needs to compute a real dx.
    // Pattern is the one used by InfiniTime's CST816S driver (the most
    // polished community implementation). Non-fatal if it NACKs.
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(0xEC));
    Wire.write(static_cast<uint8_t>(0x06));
    const uint8_t mask_err = Wire.endTransmission();
    if (mask_err != 0) {
        log_w("[cst820] MotionMask write NACKed (err=%u) — swipes may "
              "register a single press point with no motion", mask_err);
    }

    // Step 6 (round 65): enable touch / change / motion IRQs.
    //
    // Round-64 bench showed the chip going silent through entire slow
    // touches — emitted one coord at press, then nothing at all (no
    // motion samples, no gesture byte, no lift event) for the full
    // 1.2 s hold-through window. This is the chip's default IRQ
    // behaviour: register 0xFA (IRQ_CTL) ships with most of its
    // event-source bits cleared, so even though MotionMask=0x06 says
    // "report continuous slide", the chip's internal sample-update
    // path isn't gated on motion — it just stops.
    //
    //   0xFA bit 4  EnTouch  — IRQ on each touch
    //   0xFA bit 5  EnChange — IRQ on press/release transitions
    //   0xFA bit 6  EnMotion — IRQ on motion samples
    //
    // Writing 0x70 (bits 4+5+6) is the value ESPHome's production
    // cst816 driver uses verbatim. Even with our polled read_cb (we
    // don't yet hook TP_INT as a real ISR), enabling all three event
    // sources keeps the chip's register-update pipeline running
    // through the whole touch instead of falling into tap-detection
    // silence at ~200 ms. Non-fatal if it NACKs.
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(0xFA));
    Wire.write(static_cast<uint8_t>(0x70));
    const uint8_t irq_err = Wire.endTransmission();
    if (irq_err != 0) {
        log_w("[cst820] IRQ_CTL write NACKed (err=%u) — chip may stop "
              "updating registers mid-touch", irq_err);
    }

    // Step 7 (round 65): read chip ID for diagnostics. Register 0xA7
    // returns 0xB7 on a real CST820 (per ESPHome's chip-ID table).
    // Other CST816 family parts: CST816S=0xB4, CST816T=0xB5,
    // CST816D=0xB6, CST826=0x11, CST836=0x13, CST716=0x20. If we ever
    // get a board variant that ships a different sibling chip the
    // monitor trace will show it immediately.
    uint8_t chip_id = 0;
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(0xA7));
    if (Wire.endTransmission() == 0 &&
        Wire.requestFrom(static_cast<int>(TP_I2C_ADDR), 1) == 1) {
        chip_id = Wire.read();
    }

    log_i("[cst820] ok (addr=0x%02X, INT=GPIO%d, chip_id=0x%02X%s, "
          "auto-sleep disabled, MotionMask=0x06, IRQ_CTL=0x70)",
          TP_I2C_ADDR, TP_PIN_INT, chip_id,
          chip_id == 0xB7 ? " [CST820]" : " [unknown]");
    ready_ = true;
    return true;
}

bool Cst820::read(uint16_t* x, uint16_t* y,
                  uint8_t* gesture, bool* lift_event) {
    if (!ready_ || x == nullptr || y == nullptr) return false;
    if (lift_event != nullptr) *lift_event = false;

    // Round 60: periodically re-write MotionMask (0xEC = 0x06) so the
    // chip's continuous-slide config survives any auto-sleep cycles
    // that may have dropped it. The round-59 trace showed the first
    // touch after a long idle occasionally produced dx=0 dy=0 even
    // though the boot-time write succeeded — symptomatic of the chip
    // having defaulted MotionMask back to 0 during a power-save
    // dropout. Refreshing every 2 s costs one ~150 µs I2C write and
    // is well below the chip's typical sleep cycle.
    //
    // Round 65: also refresh IRQ_CTL (0xFA = 0x70) on the same cadence
    // for the same reason — if MotionMask can be dropped by a sleep
    // cycle, IRQ_CTL probably can too, and losing IRQ_CTL silently
    // re-introduces the chip-silence-mid-touch failure mode.
    {
        static uint32_t last_motion_refresh_ms = 0;
        const uint32_t now = millis();
        if (now - last_motion_refresh_ms > 2000) {
            Wire.beginTransmission(TP_I2C_ADDR);
            Wire.write(static_cast<uint8_t>(0xEC));
            Wire.write(static_cast<uint8_t>(0x06));
            Wire.endTransmission();

            Wire.beginTransmission(TP_I2C_ADDR);
            Wire.write(static_cast<uint8_t>(0xFA));
            Wire.write(static_cast<uint8_t>(0x70));
            Wire.endTransmission();

            last_motion_refresh_ms = now;
        }
    }

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
    //
    // Round 38: use endTransmission() (STOP), not endTransmission(false)
    // (repeated start). The CST820 is documented to need a clean STOP between
    // the register-pointer write and the data read; round 37 used repeated
    // start (which works on most I2C devices) and the chip silently returned
    // a zeroed buffer — finger count always 0, so every tap was dropped.
    // All the canonical CST816S/CST820 community drivers send STOP between
    // the two transactions, and the cost (one extra START on the bus) is
    // negligible at our 100 kHz rate.
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(0x01));
    if (Wire.endTransmission() != 0) return false;  // STOP

    constexpr size_t kNeeded = 6;
    const size_t got = Wire.requestFrom(static_cast<int>(TP_I2C_ADDR),
                                        static_cast<int>(kNeeded));
    if (got != kNeeded) return false;

    uint8_t buf[kNeeded];
    for (size_t i = 0; i < kNeeded; ++i) buf[i] = Wire.read();

    // buf[0] = gesture, buf[1] = finger count, buf[2] = X high
    // byte where the top two bits are the touch event flag:
    //   00 = press down,  01 = lift up,  10 = contact
    const uint8_t fingers = buf[1];
    const uint8_t event   = buf[2] >> 6;

    // Round 64: expose the gesture byte ALWAYS when requested — including
    // when fingers=0. The chip sometimes latches the slide code on the
    // tick right after lift, and round 63's "only set on success path"
    // dropped those.
    if (gesture != nullptr) *gesture = buf[0];

    static uint32_t touch_log_skip = 0;
    if ((fingers != 0 || buf[0] != 0) && (touch_log_skip++ % 30) == 0) {
        log_i("[cst820] touch: gesture=0x%02X fingers=%u event=%u "
              "raw=[%02X %02X %02X %02X %02X %02X]",
              buf[0], fingers, event,
              buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
    }

    if (fingers == 0) return false;  // no finger; gesture already exposed

    // 12-bit X = (buf[2] & 0x0F) << 8 | buf[3]
    // 12-bit Y = (buf[4] & 0x0F) << 8 | buf[5]
    *x = static_cast<uint16_t>((static_cast<uint16_t>(buf[2] & 0x0F) << 8) | buf[3]);
    *y = static_cast<uint16_t>((static_cast<uint16_t>(buf[4] & 0x0F) << 8) | buf[5]);

    // Round 64: pass through the lift-event flag. Round 58 used to drop
    // event=0x01 reads entirely; round 63's bench trace showed too many
    // swipes failing because the chip went silent through the touch and
    // the lift-event coord was our only end-of-swipe signal. The caller
    // (touchReadCb) uses lift_event to update last_x/last_y to the final
    // position AND declare release immediately without waiting for the
    // hold-through-gap timeout to elapse.
    if (lift_event != nullptr) *lift_event = (event == 0x01);

    return true;
}

}  // namespace display
