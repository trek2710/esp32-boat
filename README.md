# esp32-boat

A cockpit instrument display for the boat — **two ESP32-S3s, BLE-bridged**.
A small **transmitter** board taps the boat's NMEA 2000 backbone and a
larger **receiver** board in the cockpit shows the live data on a round
touchscreen.

```
                                                        BLE GATT
                                                       ─────────►
                                                       (5 notify
                                                        chars + 1
                                                        cmd char)
 ┌─────────── NMEA 2000 backbone ───┐
 │           SN65HVD230 transceiver │       TX                    RX
 │                ↓                 │   ┌──────────┐         ┌──────────┐
 │           ┌─────────┐            │   │ ESP32-S3 │  ─────► │ ESP32-S3 │
 └──────────►│  TWAI   │────────────────►   AMOLED │         │  Round   │
             │ peripheral            │   │  1.75″  │         │  LCD 2.1″│
             └─────────┘             │   │   466²  │         │   480²   │
                                     │   └──────────┘         └──────────┘
                                     │   (in a wiring         (in the cockpit)
                                     │    locker, near
                                     │    the bus)
```

## The two boards

**Transmitter (TX)** — [Waveshare ESP32-S3-Touch-AMOLED-1.75-G](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75)
(ESP32-S3R8, 8 MB PSRAM, 16 MB flash, 1.75″ 466×466 AMOLED, AXP2101 PMIC,
QMI8658 IMU, LC76G GNSS). Connects to the **NMEA 2000** backbone through
an **SN65HVD230** CAN transceiver. Decodes incoming PGNs (or, in sim
mode, fabricates them) and publishes the resulting boat state over BLE
GATT. Onboard AMOLED shows a status display — link state, PGN/s, errors.

**Receiver (RX)** — [Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm)
(same SoC family, 2.1″ 480×480 round IPS, capacitive touch). Connects to
*nothing on the boat* — just power. Scans for the TX's BLE service UUID,
subscribes to the boat-state characteristics, and feeds the existing
LVGL UI. Has the settings page, the honeycomb PGN display, and the
swipe-paged instruments — the user-facing surface lives entirely here.

## Status

**The BLE bridge is end-to-end working on the bench, in simulation.**
Steps 1–4, 7, 7b, and 9 have shipped. The TX runs the simulator and
publishes the five boat-state channels over BLE GATT; the RX scans
for the TX by name, subscribes to all five `NOTIFY` characteristics,
and feeds the parsed values into the existing `BoatState` so the UI
renders simulated data over the wireless link with no code path
changes upstream of `NmeaBridge`. The TX has its own five-page
status display (Primary / Simulator / PGN / Settings / Communication)
and CST9217 touch swipe nav.

Remaining for v1.5 wireless complete: **step 5** — wire the
SN65HVD230 transceiver between the TX and a real NMEA 2000
backbone, swap the TX's data source from the simulator to the
NMEA2000 library. This is hardware-blocked until the cable + bus
are connected.

Two side-quests parked on this branch:

- **Step 6** (RX → TX command channel — flip the Sim toggles from
  the RX) is deferred until step 5 lands; the wire protocol's
  command characteristic is defined in `include/BoatBle.h` and
  the TX has a callback stub but nothing decodes the bytes yet.
- **Step 8** (LC76G real GPS over the AMOLED-1.75-G's onboard
  GNSS) hit a dead-end on this board variant — the I²C path
  ACKs writes but NACKs reads, and the R15/R16 UART jumpers are
  not populated. Driver code is parked in place behind a fail-
  counter so it auto-quiets at boot; soldering the jumpers will
  reactivate it via Serial1 without further code changes.

## What it shows (v1)

Four swipe-cycled screens on the RX:

1. **Main / Overview** — classic-boating wind compass (apparent-wind
   cone, true-wind triangle on the bezel), BSPD + AWS readouts inside
   a stylised boat hull, large heading box with cardinal abbreviation,
   adjustable no-go and no-stop angles for close-hauled marking.
2. **PGN honeycomb** — 19 colour-coded hex tiles, one per PGN, with
   name / value / measured Hz. Tiles fade out if the channel goes
   silent for >10× its observed interval; flash on each update.
3. **Sim / channel toggles** — per-channel GREEN/RED buttons to enable
   or silence each simulated PGN class (used both for development and
   for testing what the display does without that data).
4. **Settings** — persisted no-go / no-stop half-angles via NVS.

Decoded PGNs: GPS position (129025), COG/SOG (129026), Wind (130306),
Water Depth (128267), Sea Water Temperature (130316-Sea), Outside Air
Temperature (130316-Outside), Heading (127250), Speed Through Water
(128259). AIS PGNs (129038/39/809/810) are TODO for v1.1.

Not in v1 (see [docs/ROADMAP.md](docs/ROADMAP.md)): waypoint
navigation, map rendering, engine telemetry, attitude PGN handling.

## Quick start

### Build and flash from source

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or
   the CLI — `pip install platformio`).
2. Clone this repo:
   ```bash
   git clone https://github.com/trek2710/esp32-boat.git
   cd esp32-boat
   ```
3. The `scripts/flash.sh` helper handles both boards and protects you
   from flashing the wrong firmware to the wrong device. Both boards
   are ESP32-S3 and look identical to macOS as `/dev/cu.usbmodem*`,
   so flash.sh fingerprints them by chip MAC on first run:
   ```bash
   ./scripts/flash.sh tx     # build + flash + monitor the transmitter
   ./scripts/flash.sh rx     # build + flash + monitor the receiver
   ```
   First run for each role asks you to register the connected board's
   MAC. After that, mis-plugging the wrong board is a hard abort
   before any bytes hit flash.

If you'd rather drive PlatformIO directly:
```bash
pio run -e nmea2k_tx                       -t upload   # transmitter
pio run -e waveshare_esp32s3_touch_lcd_21_sim -t upload   # receiver (sim)
```

### Pre-built RX firmware (no toolchain needed)

The [`binaries/`](binaries/) folder contains ready-to-flash `.bin`
files for the RX board's sim and safe builds, plus three different
flash recipes (browser-based `esptool-js`, command-line `esptool.py`,
or PlatformIO). See [binaries/README.md](binaries/README.md). The TX
firmware isn't packaged yet — flash it from source with the steps
above.

## Wiring

See [docs/WIRING.md](docs/WIRING.md) for the full pinout of both
boards. In short:

- **TX** taps the NMEA 2000 bus via an SN65HVD230 transceiver
  (GPIO assignments TBC in step 5). Powered from USB for v1, planned
  12 V → 5 V buck off the bus for v2.
- **RX** has no NMEA 2000 connection. Powered from USB-C; receives
  data from the TX wirelessly over BLE GATT.

## Repo layout

```
esp32-boat/
├── platformio.ini          PlatformIO build config (RX + TX envs)
├── src/                    RX firmware — display board
│   ├── main.cpp            App entry — sets up BLE client + UI
│   ├── BoatState.{h,cpp}   Thread-safe snapshot of all instrument values
│   ├── NmeaBridge.{h,cpp}  Sim publisher (real PGN handlers move to TX)
│   ├── Ui.{h,cpp}          LVGL pages + swipe / tap nav
│   └── display/            Hand-rolled ST77916 / ST7701 QSPI driver
├── src_tx/                 TX firmware — NMEA 2000 ↔ BLE bridge
│   └── main.cpp            AXP2101 init, SH8601 AMOLED, BLE peripheral
├── include/lv_conf.h       LVGL config, shared by both envs
├── docs/                   BOM, wiring, roadmap, navigation math
├── binaries/               Pre-built RX .bin files + flash instructions
├── scripts/flash.sh        MAC-safe TX/RX build + flash + monitor helper
├── scripts/update.sh       One-shot "ship a round" — build, log, commit, push
├── scripts/package.sh      Build + copy RX binaries into binaries/
└── .github/workflows/      CI that runs `pio run` on every push
```

## Contributing / development notes

- PlatformIO targets:
  - `nmea2k_tx` — transmitter firmware (`src_tx/`)
  - `waveshare_esp32s3_touch_lcd_21_sim` — receiver, sim mode
  - `waveshare_esp32s3_touch_lcd_21_safe` — receiver, diagnostic mode
- LVGL 8.3.11 is pinned (both envs share `include/lv_conf.h`).
- Native unit tests for PGN decoding will be added under `test/` as
  the TX-side handlers grow.
- OTA updates are wired up on the RX but disabled by default — set
  `ENABLE_OTA=1` in `platformio.ini` to turn them on once you've
  added a WiFi SSID/password.

## License

MIT — see [LICENSE](LICENSE).
