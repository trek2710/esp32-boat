# Wiring

## Overview

```
 ┌─────────────── NMEA 2000 backbone (boat) ──────────────┐
 │  …  ── T-piece ──  T-piece ── T-piece ── (terminator)  │
 └───────────────┬────────────────────────────────────────┘
                 │  drop cable (Micro-C, ~1 m)
                 ▼
 ┌───────────────────────────────────────────────┐
 │  CAN-H      CAN-L      NET-V (+12V)   NET-C   │  cut end, strip, screw terminals
 │   │           │          │              │     │
 │   │           │        ┌─▼─ 1A fuse ─┐  │     │
 │   │           │        │             │  │     │
 │   │           │        ▼             │  │     │
 │   │           │    12V→5V buck       │  │     │
 │   │           │    5V out ──► USB-C  │  │     │
 │   │           │                      │  │     │
 │   ▼           ▼                      │  │     │
 │  SN65HVD230 transceiver              │  │     │
 │   CANH  CANL  VCC(3.3V)  GND        (common GND across everything)
 │    ·     ·     ·          ·                   │
 └────┼─────┼─────┼──────────┼───────────────────┘
      │     │     │          │
      │     │  from TX ESP32 3V3 pin
      │     │                │
 ┌────▼─────▼────────────────▼─────┐
 │  Waveshare ESP32-S3-Touch-      │   TRANSMITTER (wiring locker)
 │  AMOLED-1.75-G                  │   USB-powered for v1, 12V→5V buck for boat install
 │                                 │
 │  CAN_TX  ◄── GPIO (TBC, step 5) │
 │  CAN_RX  ◄── GPIO (TBC, step 5) │
 │  3V3     ──► transceiver VCC    │
 │  GND     ──► transceiver GND    │
 │                                 │
 │      BLE 5 GATT peripheral      │
 └─────────────┬───────────────────┘
               │  wireless
               ▼
 ┌─────────────────────────────────┐
 │  Waveshare ESP32-S3-Touch-      │   RECEIVER (cockpit)
 │  LCD-2.1                        │   USB-C power, no boat-side wiring
 │                                 │
 │      BLE 5 GATT central         │
 └─────────────────────────────────┘
```

## ESP32-S3 pin assignments — TRANSMITTER

Two GPIOs go between the broken-out TX header pads and the external
SN65HVD230 CAN transceiver. Specific pin choice is locked in step 5
(the BLE bridge has to come up first); the pins below are the
schematic-verified candidates.

| Signal | ESP32-S3 GPIO | Goes to | Notes |
|---|---|---|---|
| CAN TX | **TBC, step 5** | SN65HVD230 `TXD` | TWAI controller TX output. Pick from the broken-out header pads (label "SDA" + "SCL" + 3× IO + UART RX/TX). |
| CAN RX | **TBC, step 5** | SN65HVD230 `RXD` | TWAI controller RX input |
| 3V3 | `3V3` pad | SN65HVD230 `VCC` | The board's regulated 3.3 V rail powers the transceiver |
| GND | `GND` pad | SN65HVD230 `GND` + NMEA 2000 NET-C | Common ground |

### TX internal pin map (Waveshare ESP32-S3-Touch-AMOLED-1.75-G)

Decoded from the Waveshare schematic + confirmed at runtime against the
factory firmware. The board's I²C bus is shared by AXP2101 (PMIC at
0x34), TCA9554 (GPIO expander at 0x20), CST9217 (touch at 0x5a),
QMI8658 (IMU at 0x6b), PCF85063 (RTC at 0x51), ES8311 (audio codec at
0x18), ES7210 (mic ADC at 0x40), and a presumed AT24Cxx EEPROM at 0x50.

| Signal | ESP32-S3 GPIO | Notes |
|---|---|---|
| **Shared I²C bus** |||
| I2C_SDA | GPIO 15 | shared by every chip listed above |
| I2C_SCL | GPIO 14 | ↑ |
| **SH8601 / CO5300 AMOLED (QSPI)** |||
| LCD_CS    | GPIO 12 | J30 pin 17 |
| LCD_D0    | GPIO 4  | J30 pin 16 |
| LCD_D1    | GPIO 5  | J30 pin 15 |
| LCD_D2    | GPIO 6  | J30 pin 13 |
| LCD_D3    | GPIO 7  | J30 pin 12 |
| LCD_SCK   | GPIO 38 | J30 pin 14 |
| LCD_RESET | GPIO 39 | J30 pin 18, active low |
| LCD_TE    | GPIO 13 | J30 pin 11, not wired in software yet |
| LCD_VCC   | VCC3V3  | J30 pins 22/23, no AXP2101 toggle needed |
| **CST9217 touch (later, not in v1)** |||
| TP_INT    | GPIO 21 | J30 pin 30 |
| TP_RESET  | TCA9554 EXIO6 | via I²C expander, not a direct GPIO |
| **AXP2101 PMIC** |||
| AXP_IRQ   | (TBC if wired up) | not used in v1 |

> **Termination caveat:** the Waveshare SN65HVD230 module ships with an
> on-board 120 Ω terminator. NMEA 2000 backbones already have terminators
> at both ends, and adding a third creates a 60 Ω parallel termination
> that distorts the signal. **De-solder the 120 Ω resistor on the
> transceiver before connecting it to the bus.** Skipping this step is
> the #1 cause of "everything sometimes works and sometimes doesn't"
> behaviour on a freshly-installed TX.

> **Native USB-CDC quirk:** the AMOLED-1.75-G has no USB-Serial bridge —
> the Type-C goes straight to the ESP32-S3's native USB pins, and the
> chip's USB JTAG/Serial Debug Unit handles reset. There's no DTR/RTS line
> wired to RST, so esptool's default `Hard resetting via RTS pin` is a
> no-op. The TX env's `platformio.ini` uses
> `--before=usb_reset --after=hard_reset --no-stub` plus
> `upload_speed=115200` to work around the post-stub baud-rate-change
> desync on this board. If you change the env, keep those flags.

## ESP32-S3 pin assignments — RECEIVER

No CAN wiring on the RX — it gets its data wirelessly from the TX over
BLE. Only USB-C power and the internal display + touch + I/O expander
that Waveshare pre-wired.

### Pins the Waveshare board wires internally

The display + touch + I/O expander are all pre-routed on the Waveshare PCB —
you don't solder these, but the firmware has to know the assignments. They
live in `src/display/display_pins.h` so the values below and the code can't
drift. If Waveshare ships a new revision with a different pinout, update that
header and rebuild.

> **Chipset note (round 13, 2026-04-23):** our board is the **ST7701 RGB +
> TCA9554** variant of the ESP32-S3-Touch-LCD-2.1, not the ST77916 QSPI +
> CH422G variant. An on-bench I2C scan confirmed 0x15 / 0x20 / 0x51 present
> and 0x24 / 0x38 absent. Separately, the factory demo image that shipped
> on this board displayed a working settings UI on first power-up, so the
> panel + backlight + touch are fully functional; our job is pure driver
> work. The ST7701 RGB pinout below is the round-13 target; the legacy
> ST77916 QSPI table underneath is kept only as a record of what the
> (soon-to-be-deleted) stub driver is wired to.

**Display — ST7701 RGB parallel bus (active target, round 13+)**

The ST7701 is initialised over a 3-wire SPI port, then pixels are pushed
over 16 parallel RGB565 data lines with HSYNC/VSYNC/DE/PCLK timing.

| Signal | ESP32-S3 GPIO | Notes |
|---|---|---|
| RGB_HSYNC | GPIO 38 | horizontal sync |
| RGB_VSYNC | GPIO 39 | vertical sync |
| RGB_DE    | GPIO 40 | data enable (HENABLE) |
| RGB_PCLK  | GPIO 41 | pixel clock, 14 MHz |
| R0..R4    | GPIO 46, 3, 8, 18, 17 | red channel (5 bits, LSB→MSB) |
| G0..G5    | GPIO 14, 13, 12, 11, 10, 9 | green channel (6 bits, LSB→MSB) |
| B0..B4    | GPIO 5, 45, 48, 47, 21 | blue channel (5 bits, LSB→MSB) |
| LCD_RST   | via TCA9554 IO0 / EXIO1 (not a direct GPIO) | active low |
| LCD_CS    | via TCA9554 IO2 / EXIO3 (not a direct GPIO) | 3-wire-SPI CS during init |
| LCD_BL    | GPIO 6 (raw, PWM) | 20 kHz / 10-bit via `ledcSetup` + `ledcAttachPin` + `ledcWrite` |
| SPI_SCK (init) | GPIO 2 | 3-wire-SPI clock during ST7701 init only |
| SPI_SDA (init) | GPIO 1 | 3-wire-SPI data during ST7701 init only |

Source: LovyanGFX discussion #630 (working LGFX config for this exact
board, cross-checked against Waveshare product page). Round 15 corrected
the TCA9554 bit-index off-by-one (EXIO1 = bit 0, not bit 1) and moved the
backlight off the expander onto raw GPIO6 — that's what finally turned the
screen on after a dark screen through rounds 13 and 14.

**Display — legacy ST77916 QSPI (stub driver; dead on this board variant)**

| Signal | ESP32-S3 GPIO |
|---|---|
| LCD_CS   | GPIO 21 |
| LCD_CLK  | GPIO 40 |
| LCD_D0   | GPIO 46 |
| LCD_D1   | GPIO 45 |
| LCD_D2   | GPIO 42 |
| LCD_D3   | GPIO 41 |
| LCD_RST  | via TCA9554 IO1 (not a direct GPIO) |
| LCD_BL   | via TCA9554 IO0 (not a direct GPIO) |

**Shared I2C bus** (TCA9554 I/O expander, CST820 touch, PCF85063 RTC,
plus an unidentified device at 0x6B — probably a QMI8658 IMU — and one
at 0x7E that may be spurious)

| Signal | ESP32-S3 GPIO |
|---|---|
| I2C_SDA | GPIO 15 |
| I2C_SCL | GPIO 7 |

**CST820 capacitive touch**

| Signal | ESP32-S3 GPIO |
|---|---|
| TP_INT | GPIO 4 |
| TP_RST | via TCA9554 IO1 / EXIO2 |
| TP_SDA / TP_SCL | shared I2C bus above |
| TP I2C address | `0x15` |

**TCA9554 I/O expander bit assignments** (addr 0x20, round-15 mapping)

Waveshare's reference software uses 1-indexed `EXIO1..EXIO8` labels that
map to bit positions 0..7 on the TCA9554's P0..P7 lines. Round 13 read
those labels as direct bit numbers and so shifted every signal up by one;
round 15 corrected the mapping after the LGFX #630 config was re-read.

| IO channel (bit) | Waveshare label | Drives | Notes |
|---|---|---|---|
| IO0 | EXIO1 | LCD reset | active low |
| IO1 | EXIO2 | Touch reset | active low |
| IO2 | EXIO3 | LCD CS | drives the panel's 3-wire-SPI CS line during init |
| IO3..IO6 | EXIO4..EXIO7 | (unused on this rev) | no observed effect during the round-12 walking-ones scan, re-interpreted under the round-15 mapping |
| IO7 | EXIO8 | Piezo buzzer | active high — discovered in round 12 when phases B and J beeped (bit 7 is bit 7 in either indexing, so this detection held up) |

Backlight is NOT on this expander — see the `LCD_BL` row in the main
display table for the raw-GPIO6 PWM setup.

> **Revision caveat:** Waveshare has shipped at least two "ESP32-S3-Touch-LCD-2.1"
> BOMs under the same SKU — ST77916 QSPI + CH422G, and ST7701 RGB + TCA9554.
> The in-firmware I2C scan in `src/Ui.cpp` will print which addresses it found
> at boot; cross-check against this table if the screen is dark.

## NMEA 2000 drop cable pinout (Micro-C, male, looking at the pins)

```
      ┌─────────┐
      │  1   2  │     1 = shield / drain  (optional)
      │    5    │     2 = NET-S (+12 V)
      │  3   4  │     3 = NET-C (GND)
      └─────────┘     4 = NET-H (CAN-H)
                      5 = NET-L (CAN-L)
```

Conventional Micro-C drop cable colour code:
- Pin 1 shield — **bare / drain wire**
- Pin 2 NET-S — **red**
- Pin 3 NET-C — **black**
- Pin 4 NET-H — **white**
- Pin 5 NET-L — **blue**

## Power

Do **not** try to power the display board from the NMEA 2000 bus's NET-S pin —
the bus is budgeted for low-power sensors (roughly 1 A total at 12 V across the
whole backbone). Tap the 12 V feed elsewhere (dedicated fused accessory circuit)
for the buck converter. Keep the NMEA 2000 NET-C (ground) and the buck output
ground tied together so the CAN signals share a reference.

## Termination

The NMEA 2000 backbone must have **120 Ω terminators at both ends**. Production
boats usually do already. If your bus is missing one you'll see "everything
sometimes works and sometimes doesn't" behaviour — confirm with a multimeter
(60 Ω across CAN-H/CAN-L with the bus powered off = both terminators present).

## Bench testing before installing on the boat

The receiver's `SIMULATED_DATA` build flag still exists today (in the
`waveshare_esp32s3_touch_lcd_21_sim` env) but its role changes with the
v1.5 split: starting at step 4, the receiver always reads its data from
BLE, and the simulator moves entirely to the transmitter. Until then,
the sim env on the RX remains a self-contained way to develop the UI
without any TX hardware. For bench-testing the BLE path itself before
the boat-side cabling is in place, flash both boards from source and
pair them — the TX simulator (step 3+) feeds the RX over BLE in exactly
the same way the real NMEA 2000 frames eventually will.
