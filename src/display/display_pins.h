// display_pins.h — central pinout for the Waveshare ESP32-S3-Touch-LCD-2.1.
//
// ───────────────────────────────────────────────────────────────────────────
// ROUND 15 FIX (2026-04-23, after round-14 flash still dark)
//
// Round 14 built a real ST7701 RGB driver on top of esp_lcd_panel_rgb. It
// compiled, it ran, every step logged success — and the screen stayed
// black. We fetched the LovyanGFX discussion #630 config for this exact
// board a second time and found two off-by-one errors we'd been carrying
// since round 13:
//
//   1. Backlight is NOT on the TCA9554. It's on RAW GPIO6 with PWM via
//      ledcAttach(6, 20000, 10). This is consistent with our own boot-log
//      GPIO idle-state scan showing `gpio 6 : LOW (tied down — device?)`:
//      BL was sitting at 0 V the whole time because nothing in our code
//      ever drove GPIO6. The round-13 IO0=LCD_BL guess is wrong and is
//      being retired.
//
//   2. The TCA9554 EXIO naming in LGFX #630 is 1-indexed — EXIO1 = bit 0,
//      EXIO2 = bit 1, ..., EXIO8 = bit 7. We misread it as a direct bit-
//      number mapping, which shifted every signal up by one bit:
//
//              LGFX #630 label     what it means    we had it at
//              EXIO1 = LCD_RST  →  bit 0           bit 1
//              EXIO2 = TP_RST   →  bit 1           bit 2
//              EXIO3 = LCD_CS   →  bit 2           bit 3
//              EXIO8 = Buzzer   →  bit 7           bit 7 ✓ (correct)
//
//      Which means round 14's panel-reset pulse hit the TOUCH reset pin,
//      not the LCD. The ST7701 was never actually reset, so it stayed in
//      power-on idle and ignored the 3-wire SPI init sequence entirely.
//      The buzzer bit happened to already be right (bit 7 is bit 7 either
//      way), which is why the round-12 beeps led us to a correct IO7 = BUZZER
//      mapping through a wrong model.
//
// Fix this round: shift the TCA9554 bit map down by one, drop the LCD_BL
// constant, add BACKLIGHT_PIN = 6 with the PWM config as real GPIOs below.
// Everything else (RGB pin list, 3-wire SPI init GPIOs 1/2, I2C on 15/7)
// is unchanged and confirmed correct by the same LGFX #630 reference.
// ───────────────────────────────────────────────────────────────────────────
//
// CHIPSET PIVOT (rounds 10..13, 2026-04-23)
//
// Nine rounds of bring-up assumed this board was the "ST77916 QSPI + CH422G"
// variant of the ESP32-S3-Touch-LCD-2.1 product. The exhaustive 420-pair I2C
// scan in round 9 proved that wrong:
//
//   * SDA is unambiguously GPIO 15. No other pin produces any I2C ACK.
//   * The only addresses that ACK are 0x15 (CST820 touch), 0x20 (TCA9554
//     I/O expander), and 0x51 (PCF85063 RTC).
//   * 0x24 and 0x38 — CH422G's MODE + WR_IO registers — were NEVER hit.
//
// That address signature matches Waveshare's *other* "ESP32-S3-Touch-LCD-2.1"
// BOM: ST7701 RGB panel (NOT QSPI ST77916) + TCA9554 expander (NOT CH422G) +
// CST820 touch + PCF85063 RTC. So everything we've built is for the wrong
// chipset family.
//
// Pivot plan, in order:
//   round 10  — lock SDA/SCL to GPIO 15 / GPIO 7, document the pivot.  DONE.
//   round 11  — swap ch422g.cpp for tca9554.cpp (single-addr 0x20, register
//               0x03=config / 0x01=output).  DONE.
//   round 12  — brute-force TCA9554 bit-mapping diagnostic. Ran; result was
//               "no visible backlight on any phase, but the user heard a
//               short beep on phase B (all HIGH) and phase J (IO7 HIGH) —
//               so IO7 is a piezo buzzer, not a backlight bit".  DONE.
//   round 13  — reframe. User reported that when the board was first powered
//               (before any of our firmware was flashed) the factory demo
//               image showed a working settings UI on the screen. That is
//               conclusive evidence that the hardware — panel, backlight,
//               touch, all of it — is 100% functional. Our dark screen is
//               pure driver-protocol mismatch: ST77916 QSPI sequences sent
//               to an ST7701 RGB panel render as zero pixels, and without
//               a valid pixel stream the LC layer blocks all backlight
//               light, which is why no phase lit anything up.
//               Action this round: capture the real ST7701 RGB pinout +
//               corrected TCA9554 bit-mapping (sourced from the LovyanGFX
//               discussion #630 for this exact board, cross-checked against
//               the Waveshare product page), remove the now-moot bit-mapping
//               diagnostic from Ui.cpp, and line up round 14 for the actual
//               RGB driver code.  THIS REVISION.
//   round 14+ — write src/display/st7701_panel.{h,cpp}: drive the TCA9554
//               init handshake (CS high, RST pulsed low-then-high, BL on),
//               bit-bang the ST7701 3-wire SPI init sequence, bring up
//               esp_lcd_panel_rgb with the 16 parallel data lines + sync
//               signals below, allocate a 480×480×2 PSRAM framebuffer,
//               fill black, draw a test pattern. Delete st77916_panel.*.
//               Pending.
// ───────────────────────────────────────────────────────────────────────────
//
// What this board still contains (post-pivot):
//
//   - ST7701 RGB panel controller (parallel RGB565, NOT QSPI). 480×480
//     round IPS. Init is done via SPI bit-banged from the ESP32-S3's
//     3-wire serial port, pixels go out over parallel RGB.
//   - TCA9554 I2C I/O expander at 0x20 that gates LCD reset, touch reset,
//     LCD backlight — same role CH422G was playing, different chip and
//     different protocol (single I2C address, register-based).
//   - I2C bus shared between TCA9554, CST820 touch, PCF85063 RTC, all on
//     SDA=GPIO15, SCL=GPIO7.
//   - CST820 capacitive touch at 0x15 (not driven in firmware yet — v1 is
//     pixels-only).
//   - PCF85063 RTC at 0x51 (not driven in firmware yet).
//
// ───────────────────────────────────────────────────────────────────────────

#pragma once

#include <cstdint>

namespace display {

// --- ST7701 RGB parallel bus (round 13) ------------------------------------
//
// The ST7701 panel on this board runs in 16-bit parallel RGB565 mode. Init
// happens first over a 3-wire SPI link (CS is driven by TCA9554 IO3, not a
// GPIO); after init, the panel accepts pixels over the parallel RGB bus with
// HSYNC/VSYNC/DE/PCLK as its timing signals.
//
// Pin numbers below are sourced from lovyan03/LovyanGFX discussion #630,
// which documents a known-working LGFX config for this exact board
// (Waveshare ESP32-S3-Touch-LCD-2.1, ST7701 + TCA9554 variant). They cross-
// check against the I2C pins we already locked (SDA=15, SCL=7): none of the
// RGB data pins collide with our I2C bus, which is the sanity check that
// matters most since a pin-double-assignment would kill I2C the moment the
// RGB peripheral claims the line.
//
// The 16 data lines carry RGB565: R is 5 bits (R3..R7 of an 8-bit channel,
// LSBs grounded), G is 6 bits (G2..G7), B is 5 bits (B3..B7). The ST7701 is
// wired for RGB565 so only those 16 lines are brought out; the low bits of
// each channel are grounded inside the panel module.
//
// Pin list for the parallel bus. Order inside each group is LSB→MSB — i.e.
// R_PINS[0] carries the least significant red bit that's actually wired
// (bit 3 of the red channel, because bits 0..2 are grounded in the panel).

static constexpr int RGB_PIN_HSYNC = 38;
static constexpr int RGB_PIN_VSYNC = 39;
static constexpr int RGB_PIN_DE    = 40;   // data enable (a.k.a. HENABLE)
static constexpr int RGB_PIN_PCLK  = 41;   // pixel clock

// Red: 5 bits (R3..R7).  LSB → MSB.
static constexpr int RGB_PIN_R0    = 46;
static constexpr int RGB_PIN_R1    = 3;
static constexpr int RGB_PIN_R2    = 8;
static constexpr int RGB_PIN_R3    = 18;
static constexpr int RGB_PIN_R4    = 17;

// Green: 6 bits (G2..G7). LSB → MSB.
static constexpr int RGB_PIN_G0    = 14;
static constexpr int RGB_PIN_G1    = 13;
static constexpr int RGB_PIN_G2    = 12;
static constexpr int RGB_PIN_G3    = 11;
static constexpr int RGB_PIN_G4    = 10;
static constexpr int RGB_PIN_G5    = 9;

// Blue: 5 bits (B3..B7). LSB → MSB.
static constexpr int RGB_PIN_B0    = 5;
static constexpr int RGB_PIN_B1    = 45;
static constexpr int RGB_PIN_B2    = 48;
static constexpr int RGB_PIN_B3    = 47;
static constexpr int RGB_PIN_B4    = 21;

// Pixel-clock frequency in Hz. The ST7701 at 480×480 can take up to ~25 MHz
// theoretically; the LGFX community converged on 14 MHz for this specific
// Waveshare module because faster rates introduce jitter on the PCLK trace.
// Stay conservative during bring-up; we can try bumping once pixels appear.
//
// Round 28: 10 MHz → 14 MHz. REVERTS round 20's drop. Rounds 20–27 ran
// at 10 MHz with Espressif-typical porches (pulse 10/10, back 20/10,
// front 20/10), and the mid-band stripe pattern never cleared. Round 28
// also reverts the porches to FatihErtugral's sibling-2.8"-ST7701-board
// values (pulse 8/2, back 10/18, front 50/8) — see st7701_panel.cpp. The
// PCLK and porches were tuned together on FatihErtugral's working
// config, so they belong together: at 14 MHz + those porches we get
// 548 × 508 = 278,384 clocks/frame → 50 Hz refresh, which is what the
// working sibling-board config ran at.
//
// If stripes persist at 14 MHz + FatihErtugral porches, PCLK is *not*
// the variable and the next round should go after the init sequence
// again (most likely the 0xE0..0xED GIP block, which is the only major
// block we've inherited verbatim from espressif rather than Waveshare
// factory values). But we need to test one variable at a time.
static constexpr int RGB_PCLK_HZ   = 14 * 1000 * 1000;

// ST7701 3-wire SPI init bus (software-bit-banged). On this board, LCD_CS is
// on TCA9554 IO3 (see TCA9554_BIT_LCD_CS below) — it is NOT a GPIO, so we
// drive CS by writing to the expander and then toggle SDA/SCL bits on real
// GPIOs. SDA/SCL here refer to the LCD's *own* 3-wire serial port, NOT the
// shared I2C bus — they happen to be two of the R/G/B data lines, reused
// because the panel doesn't need RGB data while we're sending init commands.
// LGFX #630 uses the same trick; we'll revisit in round 14.
//
// Resolution — matches DISPLAY_WIDTH/HEIGHT in config.h; duplicated here so
// the panel driver is self-contained.
static constexpr int PANEL_WIDTH   = 480;
static constexpr int PANEL_HEIGHT  = 480;

// --- ST77916 QSPI (LEGACY — to be removed in round 14) --------------------
//
// These constants are left in place so the (dead) st77916_panel.cpp still
// compiles while we swap drivers. Nothing on this board actually responds
// to QSPI traffic; see chipset pivot note above.
//
// --- ST77916 QSPI ----------------------------------------------------------

// SPI host. IMPORTANT: on ESP32-S3 the spi_host_device_t enum is
//   SPI1_HOST=0, SPI2_HOST=1, SPI3_HOST=2, SPI_HOST_MAX=3.
// So the integer 2 means SPI3_HOST, not SPI2_HOST as an older comment in
// this file claimed. We deliberately use SPI3 here (value 2): the
// Arduino-ESP32 core ships a global `SPIClass SPI(FSPI)` whose FSPI
// alias is SPI2_HOST, and if anything in the graphics stack ever calls
// `SPI.begin()` we'd collide with it — SPI3 has no such global. Pins are
// routed through the GPIO matrix, so the assignments below don't depend
// on which SPI peripheral we target.
static constexpr int QSPI_HOST    = 2;   // SPI3_HOST on ESP32-S3

// QSPI pin numbers. CLK + 4 data lines + CS.
static constexpr int QSPI_PIN_CS   = 21;
static constexpr int QSPI_PIN_CLK  = 40;
static constexpr int QSPI_PIN_D0   = 46;
static constexpr int QSPI_PIN_D1   = 45;
static constexpr int QSPI_PIN_D2   = 42;
static constexpr int QSPI_PIN_D3   = 41;

// Clock rate for the QSPI bus. The ST77916 datasheet allows 80 MHz on QSPI;
// start conservative at 40 MHz during bring-up and bump later if pixels work.
static constexpr int QSPI_CLOCK_HZ = 40 * 1000 * 1000;

// NOTE: PANEL_WIDTH / PANEL_HEIGHT live in the RGB section above — they're
// the physical panel size, not QSPI-specific. Don't redefine them here.

// --- Shared I2C bus (TCA9554 expander + CST820 touch + PCF85063 RTC) ------

// All three on-board I2C peripherals share one bus. The real pins on this
// board rev were nailed down by the round-9 exhaustive scan: only SDA=GPIO15
// produced any ACKs, and the address pattern (0x15 / 0x20 / 0x51) matches
// Waveshare's ST7701+TCA9554 BOM exactly. SCL=GPIO7 is the Waveshare
// silkscreen convention for this product line ("GPIO7 (SCL0)" sits next to
// "GPIO15 (SDA0)" on the 4" sibling board) and will be verified by the
// targeted SCL-verification scan in Ui.cpp round 10.
//
// History: previous rounds tried (11, 10), (10, 11), (8, 9), (6, 7) — all
// dead. See the round 9 CHANGELOG entry for the scan that pinned SDA.
static constexpr int I2C_PIN_SDA   = 15;
static constexpr int I2C_PIN_SCL   = 7;
// 100 kHz (not 400 kHz). ESP32's internal pull-ups are weak (~45 kΩ) and at
// 400 kHz the bus rise time can exceed the I2C spec, producing phantom NACKs
// across ALL devices. On a Waveshare board WITH external pull-ups 400 kHz
// works, but 100 kHz works regardless — there's no user-visible difference
// since CH422G/CST820/RTC traffic is tiny. Bump later if bring-up is stable.
static constexpr uint32_t I2C_FREQ_HZ = 100 * 1000;

// --- TCA9554 I/O expander (active chip on this board rev) ------------------

// TCA9554 is a standard single-address 8-bit I2C expander. Unlike CH422G
// (which squats on multiple I2C addresses), TCA9554 uses one address and a
// register pointer — you write [reg, value] pairs. Relevant registers:
//
//   0x00  INPUT       — read the current level of each IO pin.
//   0x01  OUTPUT      — write the level each IO pin drives when configured
//                       as output. This is what we'll poke to flip
//                       LCD_RST / TP_RST / LCD_BL.
//   0x02  POLARITY    — inversion mask (leave at 0).
//   0x03  CONFIG      — per-bit direction: 1=input, 0=output. We set this
//                       to 0x00 so all 8 IO lines drive outputs.
//
// Address: 0x20 (A0=A1=A2=GND on this board; round-9 scan confirmed 0x20
// ACKs). Reference: TI TCA9554 datasheet and Waveshare's
// esp32-s3-touch-lcd-2.1 demo for the ST7701 variant.
static constexpr uint8_t TCA9554_I2C_ADDR   = 0x20;
static constexpr uint8_t TCA9554_REG_INPUT  = 0x00;
static constexpr uint8_t TCA9554_REG_OUTPUT = 0x01;
static constexpr uint8_t TCA9554_REG_POLINV = 0x02;
static constexpr uint8_t TCA9554_REG_CONFIG = 0x03;

// Which TCA9554 IO bit drives which board signal (ROUND 15 CORRECTION).
//
// Round 13 misread the LGFX #630 "EXIO1/EXIO2/EXIO3" labels as direct bit
// numbers. They are actually 1-indexed into Waveshare's Set_EXIO() helper:
// EXIO1 = bit 0, EXIO2 = bit 1, ..., EXIO8 = bit 7. This shifted every
// signal up by one in rounds 13–14, which is why round 14's panel-reset
// pulse was actually hitting the touch-reset bit and the ST7701 never came
// out of its power-on idle state.
//
// Corrected mapping, from LGFX discussion #630:
//   EXIO1 → bit 0 → LCD_RST  (was bit 1 in round 13)
//   EXIO2 → bit 1 → TP_RST   (was bit 2 in round 13)
//   EXIO3 → bit 2 → LCD_CS   (was bit 3 in round 13)
//   EXIO8 → bit 7 → Buzzer   (bit 7 in round 13 too — accidentally right
//                              because bit 7 is bit 7 in either indexing)
//
// There is no longer a TCA9554_BIT_LCD_BL. The backlight lives on RAW
// GPIO6 with PWM, not on the expander. See BACKLIGHT_PIN below.
static constexpr uint8_t TCA9554_BIT_LCD_RST = 1 << 0;  // IO0 (EXIO1) — panel reset (active low)
static constexpr uint8_t TCA9554_BIT_TP_RST  = 1 << 1;  // IO1 (EXIO2) — touch reset (active low)
static constexpr uint8_t TCA9554_BIT_LCD_CS  = 1 << 2;  // IO2 (EXIO3) — panel CS for 3-wire SPI init
static constexpr uint8_t TCA9554_BIT_BUZZER  = 1 << 7;  // IO7 (EXIO8) — piezo buzzer (active high)
// IO3..IO6 are unused on this board rev as far as we can tell — no beep,
// no visible effect during the round-12 walking-ones scan (now reinterpreted
// with the corrected bit mapping, the only phases that DID something were
// the all-HIGH phase and the IO7-alone phase, consistent with IO7 = buzzer
// and everything else being either NC or driving panel/touch reset lines
// that don't have an audible effect). Left unassigned so a future driver
// doesn't collide with anything we discover later.

// --- Raw-GPIO backlight (round 15) -----------------------------------------
//
// The LCD backlight on this board is driven by GPIO6 through a FET + LED
// driver, NOT by any TCA9554 bit. LGFX #630 configures it as
// ledcAttach(6, 20000, 10) — 20 kHz PWM, 10-bit duty. Arduino-ESP32 2.0.16
// doesn't have ledcAttach (that's 3.x); we use the equivalent
// ledcSetup(ch, freq, res) + ledcAttachPin(pin, ch) + ledcWrite(ch, duty)
// triple instead.
//
// 20 kHz is above audible range so no backlight whine, and 10-bit gives
// 1024 brightness steps — plenty for a simple day/night toggle. During
// bring-up we just drive full duty (1023) to confirm the panel is alive.
//
// Evidence that this is correct: our own round-14 boot-log idle-state
// GPIO scan reported `gpio 6 : LOW (tied down — device?)`, meaning GPIO6
// was sitting at 0 V at boot with no firmware driving it. That's exactly
// what a backlight FET gate looks like when it's not being driven: held
// low by its pull-down to keep the backlight off.
static constexpr int BACKLIGHT_PIN       = 6;
static constexpr int BACKLIGHT_PWM_CH    = 0;           // LEDC channel — first free
static constexpr int BACKLIGHT_PWM_FREQ  = 20000;       // 20 kHz (above audible)
static constexpr int BACKLIGHT_PWM_RES   = 10;          // 10-bit duty (0..1023)
static constexpr int BACKLIGHT_PWM_FULL  = (1 << 10) - 1;  // full brightness = 1023

// --- CST820 touch (wired for future use; not driven in v1) -----------------

static constexpr int TP_PIN_INT = 4;   // Touch interrupt (active low)
// TP_RST is not a direct GPIO — it's TCA9554 IO1 / EXIO2 (see
// TCA9554_BIT_TP_RST above; round 13 had this on IO2 because of the off-
// by-one EXIO-naming misread, corrected in round 15 after the LGFX #630
// config was re-read). SDA/SCL are the shared I2C bus (GPIO 15 / 7).
static constexpr uint8_t TP_I2C_ADDR = 0x15;  // CST820 default

// --- Other devices on the shared I2C bus (round 10 discovery) -------------
//
// The round-10 full 127-address scan also found ACKs at 0x6B and 0x7E. These
// weren't in our round-9 short-list, so we didn't have a story for them.
// Best guesses (not yet confirmed):
//
//   0x6B — likely QMI8658 6-axis IMU (accel + gyro). Waveshare pairs one
//          with their round touch displays on some revs for tilt/heading.
//          QMI8658's default 7-bit address is 0x6B.
//   0x7E — unknown. Could be a real device (a battery / fuel-gauge IC, a
//          boot-mode chip), or an Arduino-ESP32 hal-i2c quirk where a
//          stuck bus produces a false ACK around the 10-bit-addressing
//          reserved band (0x78..0x7F).
//
// Neither is on the v1 critical path. If 0x6B is the IMU it'd be a nice
// feed for heel/pitch readouts later, but not required to show NMEA data.
static constexpr uint8_t IMU_I2C_ADDR_GUESS      = 0x6B;
static constexpr uint8_t UNKNOWN_I2C_ADDR_GUESS  = 0x7E;

}  // namespace display
