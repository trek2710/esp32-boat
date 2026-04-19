# Roadmap

## v0 — scaffold (this repo as-is, 2026-04-19)

- [x] PlatformIO project structure
- [x] NMEA 2000 + LVGL dependencies pinned
- [x] Placeholder instrument screens
- [x] CI builds the firmware on every push
- [ ] Flash on real hardware when it arrives
- [ ] Confirm display pin assignments against the Waveshare schematic
- [ ] Confirm CAN TX/RX pin choice against the broken-out header

## v1 — instruments only (target: hardware in hand + 2 weekends)

**Goal: a working cockpit instrument that shows live NMEA 2000 data.**

- [ ] Display driver stable (no tearing, touch responsive)
- [ ] PGN 129025 / 129026 / 129029 → GPS position + SOG + COG
- [ ] PGN 130306 → wind (apparent + true)
- [ ] PGN 128267 + 130316 → depth + water temperature
- [ ] PGN 127250 + 128259 → heading + speed through water
- [ ] PGN 129038 / 129039 → AIS position reports → scrolling target list
- [ ] Swipe / tap to page between screens
- [ ] Brightness control (long-press a corner)
- [ ] Persist last screen across reboots
- [ ] Mechanical install in the cockpit
- [ ] Retire the bench harness after 1 weekend of sea trial without crashes

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
- Writing control commands onto the bus (autopilot, engine, etc.). Read-only.
- Supporting boats without an NMEA 2000 backbone (0183-only boats would need
  a separate branch — possible but not planned).
