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

// --- Shared I2C bus (CH422G + touch + RTC) ---------------------------------

// All three on-board I2C peripherals share one bus. Waveshare's public
// documentation for the ESP32-S3-Touch-LCD family states that the I2C bus
// shared by CH422G, the CST820 touch controller, and the PCF85063 RTC is
// routed to GPIO8 (SDA) and GPIO9 (SCL) on the ESP32-S3 — which happens to
// be Arduino-ESP32's default Wire pinout too, but we pass them explicitly so
// nothing depends on that default.
//
// Earlier bring-up tried GPIO11/10 (and the swapped 10/11) based on an older
// board-rev guess; an I2C scan at 100 kHz found zero devices in either order,
// which rules those pins out. 8/9 is the documented default; the auto-scan
// logic in Ui.cpp still runs through both orderings here, so if a board rev
// does swap them we'll still find the CH422G.
static constexpr int I2C_PIN_SDA   = 8;
static constexpr int I2C_PIN_SCL   = 9;
// 100 kHz (not 400 kHz). ESP32's internal pull-ups are weak (~45 kΩ) and at
// 400 kHz the bus rise time can exceed the I2C spec, producing phantom NACKs
// across ALL devices. On a Waveshare board WITH external pull-ups 400 kHz
// works, but 100 kHz works regardless — there's no user-visible difference
// since CH422G/CST820/RTC traffic is tiny. Bump later if bring-up is stable.
static constexpr uint32_t I2C_FREQ_HZ = 100 * 1000;

// --- CH422G I/O expander ---------------------------------------------------

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
