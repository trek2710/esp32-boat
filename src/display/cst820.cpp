// cst820.cpp — see cst820.h for the protocol notes.

#include "cst820.h"

#include <Arduino.h>
#include <Wire.h>

#include "display_pins.h"
#include "tca9554.h"

namespace display {

// Round 66: interrupt-driven reads.
//
// Round-65's bench trace showed the chip going silent through ~30% of
// slow touches even with IRQ_CTL=0x70 set. Every working CST816S/CST820
// driver I could find (fbiego, ESPHome, mmMicky/TouchLib, the LVGL
// forum gold-standard pattern) reads the chip ONLY when its TP_INT line
// pulses low — they don't poll. The hypothesis is that the chip's
// internal sample/gesture pipeline only progresses when its interrupt is
// being serviced; polling against an interrupt-expecting chip puts its
// state machine into modes that explain the silence.
//
// Implementation:
//   - s_irq_pending is set by the ISR on every TP_INT falling edge,
//     cleared by Cst820::read() once the chip has been read.
//   - s_irq_count bumps on every IRQ — exposed via the existing
//     rate-limited touch-log line so we can see chip event frequency
//     on the bench.
//   - read() returns false unless s_irq_pending is set, EXCEPT for
//     the periodic MotionMask/IRQ_CTL refresh which still runs every
//     tick (those writes don't depend on chip state).
//
// Initialised true so the very first read() after begin() does one
// I2C round-trip to seed last_x/last_y from whatever the chip is
// holding — without this, the first touch would have to wait for a
// fresh IRQ before the indev driver knows the chip is alive.
namespace {
volatile bool     s_irq_pending = true;
volatile uint32_t s_irq_count   = 0;

void IRAM_ATTR onTouchIrq() {
    s_irq_pending = true;
    s_irq_count++;
}
}  // namespace

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

    // Step 2 (round 66/67): configure the INT pin as INPUT_PULLUP and
    // attach a FALLING-edge interrupt. The chip pulls TP_INT low briefly
    // on every event it wants reported (press, motion sample, lift,
    // gesture); idle, it releases the line.
    //
    // Round 67: the boot-time GPIO scan in Ui.cpp shows GPIO 4 reads LOW
    // when configured as plain INPUT — i.e. there is NO external pullup
    // on this board's TP_INT trace. With the line floating-low, a FALLING
    // edge can never happen (we're already at the low state), the ISR
    // never fires, s_irq_pending stays false after boot, and read() bails
    // forever. INPUT_PULLUP enables the ESP32-S3's internal pullup so the
    // line idles HIGH and the chip's open-drain pulse-LOW creates a real
    // falling edge. The ISR just sets a flag — the next read_cb tick
    // consumes it and does the I2C round-trip outside ISR context.
    pinMode(TP_PIN_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_PIN_INT), onTouchIrq, FALLING);

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
    // Round 69: changed value 0xFF → 0xFE. The CST816S/CST820 datasheet
    // says "any non-zero value" disables auto-sleep, so 0xFF should also
    // work — but the canonical fbiego/CST816S library uses 0xFE verbatim,
    // and round-68 bench data shows the chip dropping into a silence
    // mode on isolated touches that "auto-sleep disabled" was supposed
    // to prevent. Trying the canonical value in case the chip is picky
    // about which non-zero value is written.
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(static_cast<uint8_t>(0xFE));
    Wire.write(static_cast<uint8_t>(0xFE));
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

    log_i("[cst820] ok (addr=0x%02X, INT=GPIO%d [FALLING ISR], "
          "chip_id=0x%02X%s, DisAutoSleep=0xFE, MotionMask=0x06, "
          "IRQ_CTL=0x70, poke=500ms)",
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

    // Round 67: heartbeat — log the IRQ counter every 5 s plus the live
    // TP_INT level. If interrupts are misconfigured (wrong edge, missing
    // pullup, wrong GPIO), this line shows zero growth and a stuck level
    // immediately, instead of presenting as silent dead touch like the
    // round-66 first cut did.
    {
        static uint32_t last_heartbeat_ms = 0;
        const uint32_t now_hb = millis();
        if (now_hb - last_heartbeat_ms > 5000) {
            log_i("[cst820] heartbeat: irqs=%lu, TP_INT=%s",
                  (unsigned long)s_irq_count,
                  digitalRead(TP_PIN_INT) ? "HIGH" : "LOW");
            last_heartbeat_ms = now_hb;
        }
    }

    // Round 69: periodic chip poke. Round-68 bench (one swipe per ~10 s
    // heartbeat-spaced touches) showed 0/9 swipes detected — the chip
    // emits one coord at press and goes silent for the entire touch when
    // it has been idle for several seconds. Hypothesis: even with
    // DisAutoSleep written, the chip drifts into a deeper-than-advertised
    // power state during long idles, and the first touch after wakes the
    // chip too late to capture the gesture. Polling register 0xA7 (chip
    // ID) every 500 ms keeps the I2C bus active and gives the chip a
    // recurring "you're still in service" signal. The value we read is
    // discarded; the act of reading is the point.
    {
        static uint32_t last_poke_ms = 0;
        const uint32_t now_pk = millis();
        if (now_pk - last_poke_ms > 500) {
            Wire.beginTransmission(TP_I2C_ADDR);
            Wire.write(static_cast<uint8_t>(0xA7));
            if (Wire.endTransmission() == 0 &&
                Wire.requestFrom(static_cast<int>(TP_I2C_ADDR), 1) == 1) {
                (void)Wire.read();  // discard
            }
            last_poke_ms = now_pk;
        }
    }

    // Round 66/68: the round-66 cut gated the I2C read on s_irq_pending.
    // Round-67's heartbeat trace then revealed the chip is NOT pulsing
    // TP_INT on touch events on this hardware — only ~4 IRQs at boot
    // (chip startup chatter), then nothing during touches. With the gate,
    // touch went completely dead. Round 68: drop the gate, fall back to
    // polling (which round 65 confirmed works partially). Keep the ISR
    // wired and the heartbeat live so we still measure TP_INT activity —
    // if some future register experiment ever gets the chip to fire
    // touch IRQs, the heartbeat will show it immediately and we can
    // re-enable the gate.
    (void)s_irq_pending;

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
        // Round 66: include IRQ count so the bench trace shows how
        // often TP_INT actually pulses. Compare consecutive lines to
        // estimate IRQs per touch — a healthy slow swipe should show
        // many IRQs (one per chip motion sample); the failing class
        // would show only the press IRQ before the chip goes silent.
        log_i("[cst820] touch: gesture=0x%02X fingers=%u event=%u "
              "raw=[%02X %02X %02X %02X %02X %02X] irqs=%lu",
              buf[0], fingers, event,
              buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
              (unsigned long)s_irq_count);
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
