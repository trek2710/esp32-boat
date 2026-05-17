# Bill of Materials

Two ESP32-S3 boards, one CAN transceiver, the NMEA 2000 cabling that gets
the transceiver onto the bus, and the bits to mount everything. The
**display board** (RX) lives in the cockpit; the **transmitter board** (TX)
lives near the backbone — typically in a wiring locker or engine bay —
and connects to the cockpit display wirelessly over BLE.

## Core electronics

| Part | Qty | Notes | Typical source |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-2.1 (RECEIVER) | 1 | Cockpit display. 480×480 round IPS, capacitive touch, ESP32-S3R8 (8 MB PSRAM / 16 MB flash) | waveshare.com |
| Waveshare ESP32-S3-Touch-AMOLED-1.75-G (TRANSMITTER) | 1 | NMEA 2000 → BLE bridge. 466×466 AMOLED (status only), AXP2101 PMIC, QMI8658 IMU, LC76G GNSS (G variant). ESP32-S3R8 again. | waveshare.com |
| SN65HVD230 CAN transceiver module | 1 | Now lives on the TX side. 3.3V-native CAN transceiver. Any generic breakout works. Avoid 5V-only MCP2551. **Check the 120 Ω terminator: remove it on this module if the backbone already has terminators at both ends** (it usually does — see WIRING). | Amazon, Mouser, AliExpress |
| NMEA 2000 T-piece (Micro-C) | 1 | Drops the TX off the backbone without breaking it | Ancor, Actisense, Garmin |
| NMEA 2000 drop cable, Micro-C, ~1 m | 1 | Male end into the T-piece. We'll cut the other end to wire into the transceiver. | Same brands |
| 12V → 5V buck converter, ≥ 2A | 1 | Boat bus is 12V nominal but can spike to 15V. Pick one with input up to 24–36V. The TX is USB-powered for v1 bench testing; this buck only matters when you move the TX onto the boat permanently. | Pololu D36V28F5, Mean Well SD-15A-5 |
| 1 A inline fuse + holder (ATC) | 1 | On the 12V feed, close to the tap point | Any marine chandlery |
| USB-C pigtail / panel mount | 2 | One for the RX (cockpit power), one for the TX once it goes from bench USB to the boat's 5 V rail | Amazon |
| 120 Ω resistor (terminator) | 0–1 | Only if the NMEA 2000 backbone is missing a terminator at one end. Most boats already have both. | — |
| 4-core shielded cable, ~0.5 m | 1 | CAN-H / CAN-L / +12V / GND between T-piece side and the TX-side transceiver | — |

## Mechanical

| Part | Qty | Notes |
|---|---|---|
| Round bezel / enclosure for 2.1" display | 1 | Waveshare sells a matching case; there are 3D-printable STLs on Printables / Thingiverse if you have access to a printer |
| M2 / M2.5 mounting screws, 6 mm | 4 | Board mounting |
| Gland / strain relief for cable entry | 1 | Keeps water out of the enclosure |

## Optional but nice to have

| Part | Notes |
|---|---|
| microSD card, 8–32 GB, Class 10 | Lets us log PGN traffic + store config. Required before v3 (maps). |
| 3.3V logic analyzer / cheap USB CAN sniffer (canable.io) | Invaluable for debugging when a PGN isn't showing up |
| Second SN65HVD230 + spare ESP32 | Build a bench harness to replay logged PGNs without being on the boat |

## Notes on the transceiver choice

The ESP32-S3 has a built-in TWAI (CAN 2.0B) controller. We just need a CAN
PHY/transceiver between its 3.3 V TX/RX pins and the twisted pair on the bus.
**SN65HVD230** is the standard choice — 3.3 V native, correct logic levels, no
level shifter needed. Avoid the MCP2515+MCP2551 combo (SPI-based, older, and
unnecessary overhead here).

NMEA 2000 is electrically identical to 250 kbit CAN with a specific connector
and PGN dictionary on top. The transceiver doesn't know or care.
