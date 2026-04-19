# esp32-boat

A cockpit instrument display for the boat — an **ESP32-S3** reading the boat's
**NMEA 2000** backbone and showing live data on a **2.1" round touchscreen**.

Built around the [Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm)
(480×480 IPS round display, capacitive touch, ESP32-S3 with 8 MB PSRAM / 16 MB flash)
plus an **SN65HVD230** CAN transceiver on the NMEA 2000 bus.

## Status

v1 scaffold — instruments only. Hardware ordered 2026-04-19, awaiting delivery.
The firmware compiles against the real driver stack (LVGL 8 + ttlappalainen's
NMEA 2000 library), but some display pin assignments and LVGL tweaks will need
confirmation when the board is in hand and the schematic is open.

## What it shows (v1)

Three swipe-cycled screens:

1. **Overview** — classic-boating wind compass (needle shows apparent wind
   angle, AWS in the centre), with a large SOG readout and HDG/COG below.
2. **Data** — every value from the NMEA bus laid out in a compact grid
   (LAT/LON, SOG/COG, AWA/AWS, TWA/TWS, DEPTH/TEMP, HDG/STW, AIS count).
3. **Debug** — rolling log of received NMEA 2000 PGNs with age + decoded
   summary. Useful for verifying the bus is alive.

Decoded PGNs: GPS position (129025), COG/SOG (129026), Wind (130306),
Water Depth (128267), Water Temperature (130316), Heading (127250), Speed
Through Water (128259). AIS PGNs (129038/39/809/810) are TODO for v1.1.

Not in v1 (see [docs/ROADMAP.md](docs/ROADMAP.md)): waypoint navigation,
map rendering.

## Quick start

### Flash the pre-built firmware (no toolchain needed)

The [`binaries/`](binaries/) folder contains ready-to-flash `.bin` files
for both the simulated and production builds, plus three different flash
recipes (browser-based `esptool-js`, command-line `esptool.py`, or
PlatformIO). See [binaries/README.md](binaries/README.md).

The fastest path is the web flasher:

1. Plug the ESP32-S3 into USB.
2. Open <https://espressif.github.io/esptool-js/> in Chrome/Edge.
3. Connect → pick the port → flash `binaries/esp32-boat-sim-merged.bin`
   at offset `0x0`.

### Build from source

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or the CLI — `pip install platformio`).
2. Clone this repo:
   ```bash
   git clone https://github.com/trek2710/esp32-boat.git
   cd esp32-boat
   ```
3. Build and flash with the board plugged in via USB-C:
   ```bash
   pio run -e waveshare_esp32s3_touch_lcd_21_sim -t upload
   pio device monitor
   ```
4. To re-package the pre-built binaries after your changes:
   ```bash
   ./scripts/package.sh sim
   ```

## Wiring

See [docs/WIRING.md](docs/WIRING.md) for the full pinout. In short:

```
NMEA 2000 bus (12V, CAN-H/CAN-L)
          │
   T-piece + 1m drop cable
          │
  SN65HVD230 transceiver
  ┌──────────────────────┐
  │ CANH  CANL  VCC  GND │
  │              3V3     │
  │   TX            RX   │
  └────┬─────────────┬───┘
       │             │
   GPIO15         GPIO16        ← ESP32-S3 TWAI controller
       │             │
   Waveshare ESP32-S3-Touch-LCD-2.1 (powered via 12V→5V buck on USB-C)
```

## Repo layout

```
esp32-boat/
├── platformio.ini          PlatformIO build config (board, libs, flags)
├── src/main.cpp            App entry — sets up NMEA + UI, runs the LVGL loop
├── src/config.h            Pin assignments + user-tunable constants
├── src/BoatState.{h,cpp}   Thread-safe snapshot of all latest instrument values
├── src/NmeaBridge.{h,cpp}  PGN handlers that decode N2K frames into BoatState
├── src/Ui.{h,cpp}          LVGL screens + swipe-to-change-page logic
├── docs/                   BOM, wiring, roadmap
├── binaries/               Pre-built .bin files + end-user flash instructions
├── scripts/package.sh      Build + copy binaries into binaries/
└── .github/workflows/      CI that runs `pio run` on every push
```

## Contributing / development notes

- PlatformIO target: `waveshare_esp32s3_touch_lcd_21` (defined in `platformio.ini`).
- Native unit tests for PGN decoding will be added under `test/` as the logic grows.
- OTA updates are wired up but disabled by default — set `ENABLE_OTA=1` in
  `platformio.ini` to turn them on once you've added a WiFi SSID/password.

## License

MIT — see [LICENSE](LICENSE).
