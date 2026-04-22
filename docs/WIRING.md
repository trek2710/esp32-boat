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
      │     │  from ESP32 3V3 pin
      │     │                │
 ┌────▼─────▼────────────────▼──┐
 │     Waveshare ESP32-S3-      │   (powered via USB-C from the 5V buck)
 │     Touch-LCD-2.1            │
 │                              │
 │  CAN_TX  ◄── GPIO15 ────►    │   TX (ESP32) → TXD (transceiver)
 │  CAN_RX  ◄── GPIO16 ────►    │   RX (ESP32) ← RXD (transceiver)
 │  3V3     ──► transceiver VCC │
 │  GND     ──► transceiver GND │
 └──────────────────────────────┘
```

## ESP32-S3 pin assignments

### Pins we add (CAN transceiver)

These go between the broken-out header and the external SN65HVD230 CAN transceiver.

| Signal | ESP32-S3 GPIO | Goes to | Notes |
|---|---|---|---|
| CAN TX | **GPIO 15** | SN65HVD230 `TXD` | TWAI controller TX output |
| CAN RX | **GPIO 16** | SN65HVD230 `RXD` | TWAI controller RX input |
| 3V3 | `3V3` header pin | SN65HVD230 `VCC` | Powers the transceiver |
| GND | `GND` header pin | SN65HVD230 `GND` + NMEA 2000 NET-C | Common ground |

> **Verify before soldering:** check the Waveshare wiki schematic for
> ESP32-S3-Touch-LCD-2.1. GPIO 15 / 16 are expected to be free on the broken-out
> header, but if Waveshare is using them for something (touch INT, SD chip
> select, etc.), swap to another free pair (GPIO 6, 7, 17, 18 are common
> alternatives on ESP32-S3). Whatever you pick, update `CAN_TX_PIN` /
> `CAN_RX_PIN` in `src/config.h`.

### Pins the Waveshare board wires internally

The display + touch + I/O expander are all pre-routed on the Waveshare PCB —
you don't solder these, but the firmware has to know the assignments. They
live in `src/display/display_pins.h` so the values below and the code can't
drift. If Waveshare ships a new revision with a different pinout, update that
header and rebuild.

**ST77916 display (QSPI)**

| Signal | ESP32-S3 GPIO |
|---|---|
| LCD_CS   | GPIO 21 |
| LCD_CLK  | GPIO 40 |
| LCD_D0   | GPIO 46 |
| LCD_D1   | GPIO 45 |
| LCD_D2   | GPIO 42 |
| LCD_D3   | GPIO 41 |
| LCD_RST  | via CH422G EXIO2 (not a direct GPIO) |
| LCD_BL   | via CH422G EXIO0 (not a direct GPIO) |

**Shared I2C bus** (CH422G I/O expander, CST820 touch, PCF85063 RTC)

| Signal | ESP32-S3 GPIO |
|---|---|
| I2C_SDA | GPIO 11 |
| I2C_SCL | GPIO 10 |

**CST820 capacitive touch**

| Signal | ESP32-S3 GPIO |
|---|---|
| TP_INT | GPIO 4 |
| TP_RST | via CH422G EXIO1 |
| TP_SDA / TP_SCL | shared I2C bus above |
| TP I2C address | `0x15` |

**CH422G I/O expander bit assignments**

| EXIO channel | Drives |
|---|---|
| EXIO0 | LCD backlight enable (active high) |
| EXIO1 | Touch reset (active low) |
| EXIO2 | LCD reset (active low) |
| EXIO3 | SD card CS (not used in v1) |

> **Revision caveat:** the values above are derived from Waveshare's published
> schematic for the ESP32-S3-Touch-LCD-2.1. Some batches route LCD_RST
> directly to a GPIO instead of via CH422G, and CH422G bit-to-signal mapping
> has shifted across revisions. If you flash and see a dark screen, the first
> thing to check is whether your board rev matches this table.

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

The firmware has a `SIMULATED_DATA` build flag (see `src/config.h`). Set it
before the hardware arrives to fake NMEA values so you can iterate on the UI
purely on the desk.
