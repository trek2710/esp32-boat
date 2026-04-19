# Bill of Materials

You've ordered the display board. This list covers **everything else** you need
to connect it to the NMEA 2000 backbone on the boat and mount it in the cockpit.

## Core electronics

| Part | Qty | Notes | Typical source |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-2.1 | 1 | **Already ordered** — 480×480 round IPS, capacitive touch, ESP32-S3R8 (8 MB PSRAM / 16 MB flash) | waveshare.com |
| SN65HVD230 CAN transceiver module | 1 | 3.3V-native CAN transceiver. Any generic breakout works. Avoid 5V-only MCP2551. | Amazon, Mouser, AliExpress |
| NMEA 2000 T-piece (Micro-C) | 1 | Drops the display off the backbone without breaking it | Ancor, Actisense, Garmin |
| NMEA 2000 drop cable, Micro-C, ~1 m | 1 | Male end into the T-piece. We'll cut the other end to wire into the transceiver. | Same brands |
| 12V → 5V buck converter, ≥ 2A | 1 | Boat bus is 12V nominal but can spike to 15V. Pick one with input up to 24–36V. | Pololu D36V28F5, Mean Well SD-15A-5 |
| 1 A inline fuse + holder (ATC) | 1 | On the 12V feed, close to the tap point | Any marine chandlery |
| USB-C pigtail / panel mount | 1 | For the buck output → the display board | Amazon |
| 120 Ω resistor (terminator) | 0–1 | Only if the NMEA 2000 backbone is missing a terminator at one end. Most boats already have both. | — |
| 4-core shielded cable, ~0.5 m | 1 | CAN-H / CAN-L / +12V / GND between T-piece side and transceiver | — |

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
