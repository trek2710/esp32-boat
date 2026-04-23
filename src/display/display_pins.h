// display_pins.h — central pinout for the Waveshare ESP32-S3-Touch-LCD-2.1.
//
// ───────────────────────────────────────────────────────────────────────────
// CHIPSET PIVOT (round 10, 2026-04-23)
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
//   round 10 (this file)  — lock SDA/SCL, document TCA9554/ST7701 pivot,
//                           leave CH422G symbols so ch422g.cpp still compiles
//                           while we cut it over.
//   round 11  — swap ch422g.cpp for a TCA9554 driver (single-addr 0x20, with
//               registers 0x03=config and 0x01=output).
//   round 12+ — rip out ST77916 QSPI driver; replace with esp_lcd_panel_rgb
//               ST7701 driver + PSRAM framebuffer. Change QSPI_* pin symbols
//               below into RGB pin symbols (hsync/vsync/de/pclk/16 data
//               lines — different pinout entirely).
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

// ST77916 panel resolution (matches DISPLAY_WIDTH/HEIGHT in config.h). These
// live here too so the panel driver is self-contained.
static constexpr int PANEL_WIDTH   = 480;
static constexpr int PANEL_HEIGHT  = 480;

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

// Which TCA9554 IO bit drives which board signal. Bit positions in the
// output/config registers. Derived from Waveshare's ST7701 demo:
//   IO0 — DISP / LCD backlight enable (active high)
//   IO1 — TP_RST (capacitive touch reset, active low)
//   IO2 — LCD_RST (panel reset, active low)
//   IO3 — SD CS (we don't use SD in v1)
// TBD: verify bit ordering on YOUR board rev — some Waveshare revs swap
// these. The TCA9554 pivot + pin mapping will be empirically confirmed
// during round 11 bring-up once the TCA9554 driver lands.
static constexpr uint8_t TCA9554_BIT_LCD_BL  = 1 << 0;  // IO0 — backlight
static constexpr uint8_t TCA9554_BIT_TP_RST  = 1 << 1;  // IO1 — touch reset
static constexpr uint8_t TCA9554_BIT_LCD_RST = 1 << 2;  // IO2 — panel reset
static constexpr uint8_t TCA9554_BIT_SD_CS   = 1 << 3;  // IO3 — sd card (unused)

// --- CH422G I/O expander (UNUSED on this board rev — kept to compile) ------
// Round-9 scan proved the board does NOT host a CH422G: 0x24 and 0x38 never
// ACKed on any pin pair, whereas 0x20 (TCA9554) did. These symbols stay here
// temporarily so src/display/ch422g.cpp still compiles while we port it to
// TCA9554 in round 11. DO NOT wire new code against CH422G_*.
//
// Original CH422G docs below for reference while porting:
//
// CH422G is unusual: it doesn't use one I2C address with a register pointer,
// it uses *multiple* 7-bit I2C addresses, each one mapped to a specific
// internal register. We only need two of them:
//
//   0x24  SYS/Mode register.
//         Bit 0 (IO_OE) = 1 → make the IO0..IO7 pins outputs.
//                         Without this write the expander stays in all-
//                         input mode and the downstream LCD_RST/LCD_BL/
//                         TP_RST stays floating. Waveshare's demo writes
//                         0x01 here during init.
//
//   0x38  WR_IO register.
//         Write a byte whose bits drive IO0..IO7 directly.  Used every
//         time we flip backlight/reset. This is DIFFERENT from the CH422G
//         "OD" register (address 0x23) which drives the open-drain-only
//         EXIO0..EXIO7 pins. On the Waveshare ESP32-S3-Touch-LCD-2.1
//         board, LCD_RST/TP_RST/LCD_BL/SD_CS are wired to IO0..IO3 (not
//         to EXIO0..EXIO3), so we use 0x38.
//
// Reference: WCH CH422G datasheet + Waveshare's
// esp32-s3-touch-lcd-2.1 demo (CH422G_Mode=0x24, CH422G_WR_IO=0x38).
static constexpr uint8_t CH422G_I2C_ADDR_MODE   = 0x24;
static constexpr uint8_t CH422G_I2C_ADDR_OUTPUT = 0x38;

// Which CH422G EXIO channel drives which board signal. Bit positions in the
// CH422G's single-byte output register. Derived from Waveshare's demo code:
//   EXIO0 — DISP / LCD backlight enable (active high)
//   EXIO1 — TP_RST (capacitive touch reset, active low)
//   EXIO2 — LCD_RST (panel reset, active low)
//   EXIO3 — SD CS (we don't use SD in v1)
// TBD: verify bit ordering on YOUR board rev — some revs swap these.
static constexpr uint8_t CH422G_BIT_LCD_BL  = 1 << 0;  // EXIO0 — backlight
static constexpr uint8_t CH422G_BIT_TP_RST  = 1 << 1;  // EXIO1 — touch reset
static constexpr uint8_t CH422G_BIT_LCD_RST = 1 << 2;  // EXIO2 — panel reset
static constexpr uint8_t CH422G_BIT_SD_CS   = 1 << 3;  // EXIO3 — sd card (unused)

// --- CST820 touch (wired for future use; not driven in v1) -----------------

static constexpr int TP_PIN_INT = 4;   // Touch interrupt (active low)
// TP_RST and TP_SDA/TP_SCL are not direct GPIOs — TP_RST is EXIO1 on CH422G;
// SDA/SCL are the shared I2C bus above.
static constexpr uint8_t TP_I2C_ADDR = 0x15;  // CST820 default

}  // namespace display
