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

These are the pins **we** add on top of the display board. The display + touch
pins are handled by the board + LovyanGFX driver; you don't wire them.

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
