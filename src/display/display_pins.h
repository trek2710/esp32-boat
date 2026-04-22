// display_pins.h — central pinout for the Waveshare ESP32-S3-Touch-LCD-2.1.
//
// This board glues a LOT of stuff together. Everything we have to poke from
// firmware lives here so you never have to hunt:
//
//   - QSPI bus to the ST77916 panel controller (480×480 round IPS, 16 MB)
//   - CH422G I2C I/O expander that gates LCD reset, touch reset, and
//     LCD backlight (these are NOT direct ESP32 GPIOs on this board —
//     flipping a real reset requires an I2C write first)
//   - I2C bus shared between CH422G, CST820 touch, PCF85063 RTC
//   - CST820 capacitive touch (not wired in firmware yet — v1 is pixels-only)
//
// ───────────────────────────────────────────────────────────────────────────
// IMPORTANT — these pins are derived from Waveshare's public documentation
// for the "ESP32-S3-Touch-LCD-2.1" board as of April 2026. Waveshare has
// shipped multiple hardware revisions of this product and some have moved
// pins around (notably LCD_RST has been seen both direct and via CH422G).
// If you flash and get a dark screen, print the board revision and cross-
// check against Waveshare's schematic for YOUR rev before assuming firmware
// bugs. Every value in this file is tagged "// TBD:" or plain so you can
// grep for them during bring-up.
// ───────────────────────────────────────────────────────────────────────────

#pragma once

#include <cstdint>

namespace display {

// --- ST77916 QSPI ----------------------------------------------------------

// SPI host. SPI2_HOST is GP-SPI2, the user-accessible SPI master on ESP32-S3.
// (SPI3 / SPI1 are either used by the flash or unavailable for QSPI.)
static constexpr int QSPI_HOST    = 2;   // SPI2_HOST numeric value

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

// --- Shared I2C bus (CH422G + touch + RTC) ---------------------------------

// All three on-board I2C peripherals share one bus. Arduino's Wire default is
// SDA=GPIO8, SCL=GPIO9 which is wrong for this board — we pass these pins
// explicitly when initializing the bus.
static constexpr int I2C_PIN_SDA   = 11;
static constexpr int I2C_PIN_SCL   = 10;
static constexpr uint32_t I2C_FREQ_HZ = 400 * 1000;

// --- CH422G I/O expander ---------------------------------------------------

// 7-bit I2C address of the CH422G. Waveshare's reference code uses 0x24
// for the base (output) register; the chip also responds on 0x23 (input),
// 0x22 (mode), and 0x20 (command). See ch422g.h for the address map.
// Only the OUTPUT address is needed to drive our three control lines.
static constexpr uint8_t CH422G_I2C_ADDR_OUTPUT = 0x24;
static constexpr uint8_t CH422G_I2C_ADDR_CMD    = 0x24 >> 1; // 0x12 on some refs
static constexpr uint8_t CH422G_I2C_ADDR_MODE   = 0x24 >> 1; // placeholder

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
