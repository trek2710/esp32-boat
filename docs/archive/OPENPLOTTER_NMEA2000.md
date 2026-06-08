# OpenPlotter as the NMEA 2000 source

A standalone briefing for a future agent / future-me picking up the
**step 5 bring-up**: connecting the esp32-boat TX board to a real
NMEA 2000 backbone and proving that real PGNs flow end-to-end into
the RX. Hardware is in hand but the bus has no live sensors yet, so
OpenPlotter is being used to inject the traffic.

This is its own context — read it in a fresh session that doesn't
need to carry the BLE bridge / LVGL UI / firmware-architecture work
in head.

## What this is solving

The TX firmware compiles and runs the simulator over BLE today. To
prove the production path, we need real PGNs hitting the
SN65HVD230 → ESP32-S3 TWAI path. The boat has no installed sensors
(wind transducer, depth, etc.) yet, so the only available source of
real bus traffic is OpenPlotter on a Raspberry Pi acting as a
sender. Once that loop closes (OpenPlotter → backbone → TX → BLE →
RX → display), step 5 is done and we can move on to physical sensor
install when those parts arrive.

## What hardware is on hand

- **NMEA 2000 backbone cable + drop cable + power tap.** Standard
  Micro-C connectors. Both end terminators present (assume yes; if
  the user is unsure, multimeter across CAN-H/L should read ~60 Ω
  with the bus de-energised — that's two 120 Ω terminators in
  parallel).
- **No sensors connected to the backbone yet.** Bus is electrically
  alive once powered but carries no PGN traffic.
- **OpenPlotter setup on a Raspberry Pi.** Software stack — Signal K
  server, kplex, OpenCPN, etc. Pi model unknown; assume Pi 4 or
  newer with USB ports available.
- **The esp32-boat TX board** (Waveshare ESP32-S3-Touch-AMOLED-1.75-G,
  USB-C powered for bench testing).
- **SN65HVD230 CAN transceiver module** (3.3 V native, see
  [BOM.md](BOM.md)). Not yet wired to anything; TX-side pins to use
  are picked in step 5 of [ROADMAP.md](ROADMAP.md) (not yet committed
  to specific GPIOs — TWAI on the S3 is flexible).
- **The esp32-boat RX board** (Waveshare ESP32-S3-Touch-LCD-2.1),
  already flashed with the `_ble` env and known to subscribe to the
  TX over BLE.

## What hardware is NOT yet on hand (likely)

- A USB-to-NMEA-2000 gateway for the Pi. OpenPlotter does **not**
  natively emit NMEA 2000 over the air; it speaks Signal K
  internally, NMEA 0183 trivially, and N2K only via specific
  hardware gateways. This is the most likely missing piece.
  Candidates:
  - **Actisense NGT-1** — gold standard, ~€350, well-supported by
    canboat and Signal K.
  - **Yacht Devices YDNU-02** — about half the price, similar
    feature set, USB.
  - **canable.io / candleLight USB-to-CAN adapter** + canboat's
    `n2k-can-receiver` / `actisense-serial` style driver. Cheapest
    path (~€30), but more software setup; the kernel needs the
    `gs_usb` driver and the user must configure SocketCAN
    (`ip link set can0 up type can bitrate 250000`).
  - **PiCAN2 / PiCAN-M HAT** — directly mounts on the Pi GPIO
    header, exposes CAN as `can0` via SocketCAN. The "-M" variant
    has the Micro-C cabling on the HAT itself, removing a level
    of pigtail. ~€60–€80.

  Confirm with the user which (if any) of these they have or want
  to buy before designing the integration further.

## How OpenPlotter emits NMEA 2000

The pipeline OpenPlotter exposes:

```
boat sensors (NMEA 0183, NMEA 2000, I²C, USB GPS, etc.)
        ↓
   Signal K server (the heart of OpenPlotter — normalises everything
                    to its internal data model)
        ↓
   output plugins
        ├── signalk-to-nmea0183 → NMEA 0183 stream
        ├── signalk-to-nmea2000 → calls a CAN backend (canboat /
        │                          SocketCAN) to write PGNs
        └── signalk-server-node native PGN out (if installed)
        ↓
   USB N2K gateway / CAN HAT
        ↓
   NMEA 2000 backbone
```

The `signalk-to-nmea2000` plugin is the one we care about. It maps
Signal K paths (e.g. `environment.wind.speedApparent`) to specific
PGN encoders inside canboat (`actisense-serial` for NGT-1-style
gateways; SocketCAN for HAT-style or canable). Defaults cover the
common PGNs — wind, heading, GPS, depth, water temp — which is
exactly the set we already decode.

There's also `signalk-to-canboat` and the `n2kanalyzer` family of
tools — same idea, different glue. The user has used OpenPlotter
before (their phrasing: "may be able to get it to output NMEA 2K"),
so they likely have at least Signal K running.

## Open questions to resolve in the first session

Ask the user, in roughly this order:

1. **What hardware connects the Pi to the bus today?** None, USB
   gateway (which model?), or CAN HAT (which model?). Determines
   100% of the rest.
2. **What's already running on OpenPlotter?** Signal K server up?
   Any data sources configured? Any sample / synthetic data
   plugins installed?
3. **Does the user want OpenPlotter to inject *simulated* sensor
   data**, or are they planning to feed it from a NMEA 0183 source
   (e.g. an old chartplotter) that gets converted up to NMEA 2000?
   The former is faster to set up; the latter is closer to real-
   boat conditions.
4. **What's the physical layout?** Does the Pi sit next to the TX
   and the backbone, or is one of them remote? Affects whether
   we're testing on a bench (everything on a desk) or partially
   installed.

## Suggested first session structure

1. **Verify the bus is alive.** Power the backbone (~12 V on the
   red/black pair). Multimeter across CAN-H/L: ~60 Ω with bus
   off, ~2.5 V on both with bus on. If either reading is wrong,
   stop and diagnose cabling before doing anything else.
2. **Get the Pi onto the bus.** Install + configure the chosen
   gateway/HAT. For SocketCAN-based paths:
   ```bash
   sudo ip link set can0 up type can bitrate 250000
   candump can0    # should show nothing yet — empty bus
   ```
   For Actisense-style USB gateways:
   ```bash
   actisense-serial -s 115200 /dev/ttyUSB0 | analyzer
   ```
3. **Get Signal K outputting PGNs.** Install the
   `signalk-to-nmea2000` plugin via the Signal K admin UI. Enable
   the PGNs the esp32-boat decodes (129025 GPS position, 129026
   COG/SOG, 130306 Wind, 128267 Depth, 130316 Sea/Air Temp,
   127250 Heading, 128259 STW). Use a Signal K synthetic data
   plugin (e.g. `signalk-fake-data`) or hand-injected values via
   the admin UI to drive them with known numbers.
4. **Verify PGNs on the wire.** `candump can0 | grep <PGN>` (after
   converting PGN to CAN-ID range — canboat's `analyzer` tool
   does this nicely). Confirm each PGN is being emitted at its
   spec cadence.
5. **Wire the TX into the backbone.** Use a T-piece + drop cable
   to the SN65HVD230. CAN-H/CAN-L on the bus pair, 12 V → TX 5 V
   buck, GND common. **Check terminator first** — most factory
   modules ship with a 120 Ω terminator soldered on, and a third
   terminator on an already-terminated bus pulls the impedance
   too low. Remove if present.
6. **Bring up the TX with `-DSIMULATED_DATA=0`.** Currently the
   TX env has the simulator wired in via `simulateTick()`. Step
   5 will introduce a new build flag that swaps the simulator
   for `NMEA2000.ParseMessages()` calls feeding the same
   `BoatState` setters. The handlers already exist in
   `src/NmeaBridge.cpp` (RX side) — most of the work is
   porting / mirroring them on the TX so they call the existing
   BLE publishers instead of UI setters.
7. **Verify end-to-end.** RX should show the Signal K-injected
   values on the existing UI (Main page wind compass, depth
   readout, etc.). Per-channel notify counters on the TX
   Communication page should climb at the Signal K cadence,
   not the simulator cadence.

## Key references

- canboat — https://github.com/canboat/canboat — the de-facto PGN
  encoder/decoder library; underpins almost every open-source
  N2K project.
- Signal K — https://signalk.org/ — data model docs explain what
  Signal K *paths* map to which N2K *PGNs*.
- `signalk-to-nmea2000` plugin —
  https://github.com/SignalK/signalk-to-nmea2000 — the actual
  PGN-out source on the Pi side.
- ttlappalainen/NMEA2000_esp32_twai —
  https://github.com/ttlappalainen/NMEA2000_esp32_twai — the
  library the TX firmware will use to read PGNs off the
  transceiver. Use this fork (not the ESP32-classic NMEA2000_esp32)
  on the ESP32-S3 — it uses the TWAI peripheral, which is
  the only CAN controller the S3 actually has.
- Waveshare ESP32-S3-Touch-AMOLED-1.75-G schematic — already on
  hand from earlier sessions. The TWAI peripheral has flexible
  pin assignment; pick free GPIOs for CAN_TX / CAN_RX in the
  initial wiring. GPIOs 4–13 are used by the QSPI display, GPIOs
  14/15 by I²C, GPIO 17/18 reserved for LC76G UART even though
  unpopulated. GPIOs 1, 2, 3, 8, 11, 12 (etc.) are free per the
  schematic — confirm against the breakout header pins that are
  actually accessible before soldering.

## What does NOT belong in this session

- BLE protocol changes (`include/BoatBle.h`) — locked, don't
  modify.
- UI work on the RX. The RX Communication page already shows the
  numbers we need to debug step 5.
- LVGL / display driver work on the TX. The TX UI shows BLE
  state, not N2K state; once `NmeaBridge`-on-TX is feeding
  `BoatState` correctly, the existing notify counters update
  automatically.
- Step 8 (LC76G GPS). Dead-end on this board variant. Use the Pi
  + Signal K to inject GPS instead — that's cleaner anyway since
  you control the test values.
