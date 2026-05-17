# Roadmap

## v0 — scaffold (this repo as-is, 2026-04-19)

- [x] PlatformIO project structure
- [x] NMEA 2000 + LVGL dependencies pinned
- [x] Placeholder instrument screens
- [x] CI builds the firmware on every push
- [ ] Flash on real hardware when it arrives
- [ ] Confirm display pin assignments against the Waveshare schematic
- [ ] Confirm CAN TX/RX pin choice against the broken-out header

## v1 — instruments only (single board, target: hardware in hand + 2 weekends)

**Goal: a working cockpit instrument that shows live NMEA 2000 data.**

- [x] Display driver stable (no tearing, touch responsive)
- [x] PGN 129025 / 129026 / 129029 → GPS position + SOG + COG
- [x] PGN 130306 → wind (apparent + true)
- [x] PGN 128267 + 130316 → depth + water temperature
- [x] PGN 127250 + 128259 → heading + speed through water
- [ ] PGN 129038 / 129039 → AIS position reports → scrolling target list
- [x] Swipe / tap to page between screens
- [ ] Brightness control (long-press a corner)
- [ ] Persist last screen across reboots
- [ ] Mechanical install in the cockpit
- [ ] Retire the bench harness after 1 weekend of sea trial without crashes

## v1.5 — wireless split (in progress)

**Goal: move from one all-in-one board to two BLE-bridged boards.**

The receiver stays in the cockpit; a new transmitter board lives near
the NMEA 2000 backbone (wiring locker / engine bay) and forwards data
over BLE GATT. The SN65HVD230 transceiver moves off the RX onto the
new TX board. The simulator follows it, so sim and real PGNs flow
through the same wireless link.

- [x] **Step 1a** — PIO `[env:nmea2k_tx]`, src_tx scaffold, serial-only
      Hello on the Waveshare ESP32-S3-Touch-AMOLED-1.75-G
- [x] **Step 1b** — AXP2101 PMIC init via XPowersLib (SDA=GPIO15 /
      SCL=GPIO14), full I²C bus census, live battery / VBUS telemetry
- [x] **Step 1c** — SH8601 AMOLED brought up over QSPI via Arduino_GFX
      (CO5300 driver, which is pin-compatible), LVGL 8.3.11 bound via
      a flush callback into a 466×466 PSRAM buffer, centred
      "Hello, transmitter" label rendering
- [x] **Step 2** — `include/BoatBle.h` shared wire protocol —
      service UUID + 5 notify characteristics (Wind / GPS / Heading /
      Depth-Temp / Attitude) + 1 command characteristic. All PDUs
      packed, fit inside the default 23-byte ATT MTU, valid-mask
      bitmap per channel so missing fields are explicit instead of
      sentinel-encoded.
- [x] **Step 3** — TX runs the simulator + acts as BLE peripheral
      (`esp32-boat-tx`), notifying boat-state on the five channels
      at NMEA-2000 spec cadences. Subscribed counts visible on the
      TX's Communication page; verified subscribable from nRF Connect.
- [x] **Step 4** — RX-side BLE central scans for the service UUID,
      auto-connects to the first `esp32-boat-tx` it sees, subscribes
      to all five characteristics, parses PDUs into existing BoatState
      setters. On disconnect `BoatState::invalidateLiveData()` blanks
      all live fields to NaN so the UI renders "—" instead of stale
      values. Status getters expose link state / peer MAC / RSSI /
      per-channel notify counters, shown on a new RX Communication
      page (page 6 under `DATA_SOURCE_BLE`). New PIO env
      `[env:waveshare_esp32s3_touch_lcd_21_ble]` gates the central
      backend on `-DDATA_SOURCE_BLE=1`.
- [ ] **Step 5** — TX wired to NMEA 2000 bus through SN65HVD230,
      NMEA2000 + NMEA2000_esp32_twai libraries forward real PGNs
      onto the same notify path. **Hardware-blocked** until the
      transceiver is wired and the bus has a live source. See
      [OPENPLOTTER_NMEA2000.md](OPENPLOTTER_NMEA2000.md) for the
      OpenPlotter-as-source bring-up path.
- [ ] **Step 6** — RX → TX command channel: page-3 sim toggles fire
      writes back to the TX. Wire format reserved in `BoatBle.h`,
      `CommandCallbacks::onWrite` in `src_tx/main.cpp` logs bytes
      but doesn't decode yet. Deferred until step 5 lands.
- [x] **Step 7** — TX status display on the AMOLED, five pages
      (Primary, Simulator, PGN, Settings, Communication) with
      BLE link state, per-channel notify rate, AXP2101 telemetry,
      uptime. Read-only — Sim toggles surface state but don't yet
      flip mask (waits on step 6).
- [x] **Step 7b** — CST9217 capacitive touch via lewisxhe/SensorLib;
      swipe-left / swipe-right gestures change pages. Reset line
      driven via TCA9554 EXIO6 (CST9217 RST isn't on a direct GPIO
      on this board). SensorLib upstream has a `getPoint(0)` bounds
      bug that spams the log on every touch; patched in-place at
      build time by `scripts/patch_sensorlib.py` (idempotent,
      self-healing across libdeps wipes).
- [—] **Step 8** — Real GPS via the AMOLED-1.75-G's onboard LC76G.
      **Dead-end on this board variant.** I²C path against 0x50
      ACKs writes but NACKs reads (Quectel's app note is paid).
      UART path via GPIO17/18 stays silent — the R15/R16 0 Ω
      jumpers between LC76G TX/RX and the ESP32 pins aren't
      populated. Driver code is parked in place: I²C poll
      auto-disables itself after N consecutive NACKs, UART drain
      runs every loop iteration so soldering R15/R16 lights it up
      with no code change. Will revisit if an external GPS
      becomes attractive.
- [x] **Step 9** — TX UI polish: titles 28pt, rows 24pt, Sim page
      six category-coloured pills (display-only — interactive
      toggling lives on step 6), PGN page rows tinted by category
      pastel to mirror the RX honeycomb's colour-coding. A
      tap-to-toggle experiment was tried and reverted — the tap
      detection added a 300 ms lockout that interfered with swipes,
      so toggle UX waits for step 6.

## v2 — waypoint navigation

- [ ] Waypoint list (load/save JSON on the microSD card)
- [ ] "Go to" screen: bearing + distance + cross-track error to active WP
- [ ] Receive active waypoint from a chartplotter via PGN 129284 if present
- [ ] Send local waypoints onto the bus (PGN 129285) so they show on the chartplotter

## v3 — map

- [ ] Pre-render OSM tiles to microSD offline
- [ ] Tile cache + pan/zoom on the round display
- [ ] Boat icon heading-up, lat/lon from GPS
- [ ] AIS targets as icons on the map, not just a list

## v4 — polish

- [ ] OTA updates over WiFi (marina / phone hotspot)
- [ ] Anchor watch: alarm if the boat drifts > X metres from a dropped pin
- [ ] Trip log: miles, hours, max SOG, max wind — exported as CSV on the SD

## Non-goals

- Being a replacement for a full MFD. We are a single-purpose instrument.
- Writing **NMEA 2000 control commands** onto the boat bus (autopilot,
  engine, etc.). The bus-side stays read-only. The BLE command channel
  (RX → TX) is internal — it only configures the TX (sim toggles, future
  brightness, etc.), it never reaches the boat's N2K backbone.
- Supporting boats without an NMEA 2000 backbone (0183-only boats would need
  a separate branch — possible but not planned).
