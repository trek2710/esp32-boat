# Roadmap

> **v2 pivot (2026-06-07, ADR-0016):** the project abandoned the WiFi
> "virtual bus" / NMEA 2000 mirror entirely and became a family of
> **independent, self-contained devices** that link to an iOS companion
> over BLE. **Everything below the "v2 — independent devices" section is
> v1 history**, kept for reference and recoverable at the
> `v1-wifi-bus-archive` tag, but no longer the plan.

## v2 — independent devices over BLE ← CURRENT DIRECTION

**Goal: self-contained ESP boxes, each doing one boat job standalone,
each linked only to an iOS companion over BLE.** No shared bus, no boat
network required. Architecture: [ADR-0016](adr/0016-independent-devices-over-ble.md);
vocabulary: [CONTEXT.md](CONTEXT.md).

### Device 1 — AIS-radar device (milestone 1)

The ESP32-S3-Touch-AMOLED-1.75-G (onboard LC76G GPS) + a Daisy 2+ AIS
receiver, drawing AIS targets radar-style on its own AMOLED when it has
a fix, with a BLE link to a fresh iOS app. Self-powered.

**Salvage (keep):** AMOLED display/touch/PMIC bring-up, LC76G GPS
driver, AIS AIVDM decode (now `shared/ais/`), radar geometry/trig.
**Re-target:** the radar is an LVGL→LVGL port — both boards run LVGL
8.3.11 (the AMOLED uses Arduino_GFX only as LVGL's flush backend), so
re-aim the canvas + screen size, not a graphics-API rewrite. iOS app →
fresh, BLE central. **Drop:** WiFi bus, role election, HTTP settings.

- [x] **0. Repo reset — DONE.** Tag `v1-wifi-bus-archive`; `devices/` +
      `shared/` layout. Salvaged (all hardware-verified on the AMOLED
      board): `shared/ais/` (AIVDM decode + de-N2K'd, lat/lon-fixing
      `AisTargetDecoder`), `shared/display/` (`AmoledDisplay` panel+LVGL,
      `Tca9554` expander, `Lc76gGps` UART NMEA — dead I2C path dropped,
      streams once R15/R16 soldered, `Touch` CST9217 via SensorLib),
      `shared/ble/BoatBle.h`. `devices/ais-radar/` builds + runs:
      display + touch + GPS + AIS all live on the board. Deleted `src/`
      `src_tx/` `src_converter/` `include/` + the root `platformio.ini`
      (recoverable at the tag); CI now builds the device.
- [ ] **1. AMOLED + GPS bring-up** — display/touch up on the new tree;
      LC76G UART jumpers (R15/R16) soldered; own position acquired.
- [ ] **2. Daisy AIS in** — wire the Daisy to a spare UART; feed it into
      the on-device `AisTargetDecoder` (already building).
- [ ] **3. On-device radar** — render own ship + AIS targets radar-style
      in LVGL (re-targeted from the RX radar); touch = zoom/pan only.
- [ ] **4. BLE link** — device (peripheral) notifies AIS targets + own
      GPS + status; iOS settings write characteristic; device persists
      settings to NVS and runs standalone.
- [ ] **5. Fresh iOS app** — BLE central: radar/AIS view + the settings
      writer.

**Acceptance:** powered by its own battery/powerbank, no boat
connection, the device shows my GPS position with live AIS targets
around it radar-style; the iOS app mirrors it and can change its
settings.

### Later devices / capabilities (not scheduled)

- Comfort device (IMU sea-state → comfort index) — design in
  [ADR-0014](adr/0014-comfort-page-sea-state.md), **mechanism needs BLE
  rework** (it predates this pivot).
- Optional per-device **N2K bridging** — a device gains an SN65HVD230 to
  emit/read PGNs. The v1 encoders + ADR-0005 are kept dormant for this.
- Fate of the RX 2.1" round display and the C6 converter as future
  devices.

---

# v1 history (archived — WiFi virtual bus, superseded by ADR-0016)

Everything below predates the v2 pivot. Recoverable at
`v1-wifi-bus-archive`.

## Sea-trial v1 — standalone AIS + GPS pod (SUPERSEDED by ADR-0016)

**Goal: a self-powered box that shows real AIS + iPhone-GPS position on
the RX and iOS, on a boat with no instruments and no NMEA 2000.** The
test boat has no N2K backbone and no wind instrument, so this release
drops the "mirror a real bus" premise entirely. Rationale + the four
scoping decisions: [ADR-0015](adr/0015-standalone-pod-first.md).
"Standalone pod" deployment mode: [CONTEXT.md](CONTEXT.md).

**In scope:** iPhone GPS (foreground) for own position; real AIS via the
Daisy 2+ → converter → virtual bus; the RX radar + AIS list; the iOS map
+ list. Self-powered (LCD batteries, converter on a powerbank).

**Deferred:** comfort/IMU (ADR-0014), a dedicated GPS module, wind /
depth / heading sensors, the N2K **gateway mode**.

**Priorities (do roughly in order):**

- [ ] **1. Simulator master switch** — one `sim.master` setting gates
      *all* sim publishing (RX sim + converter AIS sim). Bench = on
      (today's behaviour); trial = one tap off so only real sources
      flow. Prevents `sim.gps` from pegging own-ship in Copenhagen.
- [ ] **2. Heading honesty** — there is no magnetometer; own-ship is
      COG-only (valid when moving). Ensure no display implies a magnetic
      bow heading; label it COG, not HDG.
- [ ] **3. Field power-on autonomy** — the three boards + iPhone form
      the virtual bus on battery alone, no laptop: RX comes up as AP
      reliably, converter + iPhone join, iOS app foreground.
- [ ] **4. Real AIS bring-up** — wire the Daisy 2+ to the converter
      (`docs/hardware/hardware_guide.html`), powerbank it, verify real
      targets travel Daisy → converter → bus → RX radar + iOS.
- [ ] **5. Real-position verification** — `sim.master` off, confirm the
      RX radar + iOS map centre on the live iPhone position with real
      AIS targets around it.

**Acceptance:** underway on the real boat, battery-powered, no laptop —
the RX radar and the iOS map show my real position with live AIS targets
around me for a full short sail.

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

## v1.5b — screen cleanup (round 85, **shipped**)

**Goal: unified 4-page LCD layout, AIS on the LCDs, drop the
half-finished pages.** Brief in [SCREEN_REDESIGN.md](SCREEN_REDESIGN.md).

- [x] **Step 1** — Flip the Main-page AWA cone so the thick end
      points outward instead of inward.
- [x] **Step 2** — Comm page rewritten as two columns (Network /
      Hardware), absorbing TX's previous Primary content.
- [x] **Step 3** — Settings / Wind-overview / Simulator pages dropped
      from RX. `kNumPages = 4`, swipe order: Main / AIS / PGN / Comm.
- [x] **Step 4** — AIS list page on RX (sorted by last-seen; renders
      from `BoatState::aisSnapshot()`).
- [x] **Step 5** — GPS-aware AIS list: when own-GPS fix is present,
      list sorts by range ascending and populates RNG / BRG columns
      via inline haversine. (Full canvas-based compass overlay
      deferred — the list path delivers the user-meaningful info.)
- [x] **Step 6** — Converter AIS filters (`ais.range_nm` /
      `ais.hide_anchored` / `ais.stale_s`) wired into
      `refreshTargets()`.
- [x] **Step 7** — TX page set converged to 4 pages (Main / AIS /
      PGN / Comm). TX now consumes incoming bus PGNs into a local
      `BoatState` (round-85 step-7-fix) and renders real values; AIS
      list mirrors RX's; Comm page is two-column.

## v1.6 — iOS companion app + settings control plane (round 85, **mostly shipped**)

**Goal: native SwiftUI app that joins the bus as a full peer and
becomes the canonical settings UI.** Design in
[IOS_APP.md](IOS_APP.md); settings control plane in
[ADR-0013](adr/0013-settings-control-plane.md).

- [x] **Step 1** — Settings control plane on the firmware: AP-side
      `WebServer` on port 80 with `GET`/`POST /settings`, heartbeat
      snapshot fan-out, NVS persistence on every ESP. Consumers wired
      for: `nav.no_go_deg` (RX Main), `sim.*` channel toggles (RX
      simulator), `ais.hide_anchored` + `ais.stale_s` (converter
      filter), `ui.brightness` (converter backlight).
- [x] **Step 2** — Xcode project (`../esp32-boat-ios/`) scaffolded
      with xcodegen, 4-tab SwiftUI shell, UDP listener + heartbeat
      publisher + PGN dispatcher all wired. Captive-portal stub on the
      AP (DNS hairpin + iOS probe URLs) so iOS classifies
      `_wifi_nmea2k` as a real network and doesn't fall back to
      cellular.
- [x] **Step 3** — Boat tab (live instrument grid + TWA/TWS/VMG
      derived locally + per-channel staleness sweep blanks cells when
      a channel goes silent).
- [x] **Step 4** — AIS tab (placeholder; populated by converter's
      AIS-sim replay over the bus). Plus: iPhone GPS publisher
      (PGN 129025, **opt-in default off**, toggle on Settings tab,
      stored in UserDefaults). Plus: Boat-tab Position cell with
      cross-check of bus-published GPS against iPhone GPS — bold
      orange when delta > 30 m.
- [x] **Step 5** — Own-ship SOG/COG available everywhere via local
      derivation from successive PGN 129025 fixes (firmware `BoatState`
      round 56; iOS `PgnDispatch.handleGps`). Per-target SOG/COG
      already rides PGN 129039 from the converter. A 129026 wire-up
      across peers was **declined**: it would contradict the round-56
      "derive, don't trust a sensor-supplied COG/SOG" rule (RX already
      ignores 129026) and add nothing the map needs.
- [ ] **Step 6** — AIS map view: SwiftUI Canvas with own ship +
      AIS targets + 5-minute forward-projection arrows. **Unblocked**
      — all inputs (own-ship position/heading/SOG/COG, per-target
      position/SOG/COG) are on the bus today. Plus Diagnostics tab
      polish (PGN-rate honeycomb, raw-log drawer).

## v1.7 — future polish (not yet planned)

- Real boat connectivity: SN65HVD230 to a live N2K backbone (v1.5
  step 5, hardware-blocked; see
  [OPENPLOTTER_NMEA2000.md](OPENPLOTTER_NMEA2000.md)).
- LC76G GPS via the AMOLED-1.75-G's onboard GNSS — requires
  soldering R15/R16 jumpers.
- TX compass widget on Main (currently text grid; RX has the full
  LVGL compass).
- iOS distribution signing ($99/yr Apple Developer Program) — the
  free-tier signing requires online cert verification at each launch.
- Wi-Fi role-priority surfacing in heartbeat (`has_transceiver`
  bool, task #45) — meaningful once multiple transceiver peers exist
  on the same physical N2K backbone.

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

## v5 — comfort / seakeeping (design only, not scheduled)

**Goal: a comfort page driven by the boats' idle QMI8658 IMUs.**
Architecture pinned in [ADR-0014](adr/0014-comfort-page-sea-state.md)
and the "Comfort / sea state" glossary in [CONTEXT.md](CONTEXT.md);
no implementation yet. In short: reconstruct a (boat-distorted) sea
state from each LCD's IMU, correct it with a learned per-boat model
held in the iOS app, and show a steepness-based comfort index — with
the active source (cockpit-measured / locker-measured / manual / GFS
forecast) user-selectable. See the ADR's "Open questions" for what's
still undecided.

## Non-goals

- Being a replacement for a full MFD. We are a single-purpose instrument.
- Writing **NMEA 2000 control commands** onto the boat bus (autopilot,
  engine, etc.). The bus-side stays read-only. The BLE command channel
  (RX → TX) is internal — it only configures the TX (sim toggles, future
  brightness, etc.), it never reaches the boat's N2K backbone.
- ~~Supporting boats without an NMEA 2000 backbone.~~ **Reversed
  (2026-05-26, ADR-0015):** the first sea-trial boat has no N2K backbone,
  so the **standalone pod** deployment mode (iPhone GPS + Daisy AIS + IMU,
  no boat connection) is now a first-class target. The N2K **gateway
  mode** remains supported for boats that do have a backbone.
