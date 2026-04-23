// display_pins.h — central pinout for the Waveshare ESP32-S3-Touch-LCD-2.1.
//
// ───────────────────────────────────────────────────────────────────────────
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

// Which TCA9554 IO bit drives which board signal.
//
// Mapping below is the round-13 revision, pieced together from two sources:
//   (a) LovyanGFX discussion #630 (this exact board): EXIO1 = LCD_RST,
//       EXIO2 = TP_RST, EXIO3 = LCD_CS. Note EXIO1 and EXIO2 are SWAPPED
//       from what we had in rounds 11–12, and EXIO3 is LCD_CS (for the
//       panel's 3-wire init bus), NOT the SD card CS.
//   (b) The round-12 bit-mapping diagnostic: phases B (all HIGH) and J
//       (only IO7 HIGH) produced a short beep. IO7 is therefore a piezo
//       buzzer driven active-high, not anything LCD-related. That's not
//       in any LGFX config we've seen but explains the phase J beep.
//
// IO0 is the only bit we never got independent confirmation on. The
// Waveshare product page says "Pin 2 of the TCA9554 ... controls the
// screen backlight" (ambiguous between IC-pin-2 and IO2). Since IO1 and
// IO2 are both committed to resets, IO0 is the only remaining candidate
// for LCD_BL on this expander, so we keep the active-high LCD_BL guess
// there. If the backlight still refuses to come on once the RGB driver
// is producing valid pixels in round 14+, we move backlight off the
// expander entirely and probe direct GPIOs.
static constexpr uint8_t TCA9554_BIT_LCD_BL  = 1 << 0;  // IO0 — backlight (active high, unconfirmed)
static constexpr uint8_t TCA9554_BIT_LCD_RST = 1 << 1;  // IO1 — panel reset (active low)
static constexpr uint8_t TCA9554_BIT_TP_RST  = 1 << 2;  // IO2 — touch reset (active low)
static constexpr uint8_t TCA9554_BIT_LCD_CS  = 1 << 3;  // IO3 — panel CS for 3-wire SPI init
static constexpr uint8_t TCA9554_BIT_BUZZER  = 1 << 7;  // IO7 — piezo buzzer (active high)
// IO4..IO6 are unused on this board rev as far as we can tell — no beep,
// no visible effect during the round-12 walking-ones scan. Left unassigned
// so a future driver doesn't collide with anything we discover later.

// --- CST820 touch (wired for future use; not driven in v1) -----------------

static constexpr int TP_PIN_INT = 4;   // Touch interrupt (active low)
// TP_RST is not a direct GPIO — it's TCA9554 IO2 (see TCA9554_BIT_TP_RST
// above; note rounds 11–12 had this on IO1 — corrected in round 13 from
// LovyanGFX discussion #630). SDA/SCL are the shared I2C bus (GPIO 15 / 7).
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
