# Screen redesign — round 85

Round-85 grilling pinned the page layout for the three displays. This
file is the implementation brief; the protocol decisions it depends on
are in [ADR-0013](adr/0013-settings-control-plane.md).

## LCD page set (both `lcd-gps` and `lcd-2.1` identical)

Four pages, swipe-cycled left/right:

| # | Page | What it shows |
|---|------|---------------|
| 1 | **Main**   | Compass overview (existing) |
| 2 | **AIS**    | Compass overlay (if own-GPS fix) **or** scrollable list (if no fix) |
| 3 | **PGN**    | 19-tile honeycomb (existing) |
| 4 | **Comm**   | Two columns — Network (left) / Hardware (right) |

Removed entirely: the standalone **Wind** overview page (Main covers
it), the **Sim** channel-mask page (moved to iOS), the **Settings**
no-go-zone page (moved to iOS).

### Main page — only change

Flip the apparent-wind cone (`main_pg.twd_arrow`, the long yellow
slim cone) so the **thick end points outward**. Today it points
inward (cone narrows toward the rim). Same colour, same length, same
binding to `s.awa`.

Code location: `src/Ui.cpp` around `buildMainPage()` line ~1265 where
`lv_meter_add_needle_img` is called for the cone, plus the static
`needle` image asset definition. Either flip the image data
horizontally or invert the value-to-angle mapping; the latter is
fewer changes.

### AIS page — new on the LCDs

Two layouts, chosen at refresh time by checking
`BoatState::hasGpsFix()`:

**With GPS fix — compass overlay.** Same outer-ring compass as Main.
For every AIS target passing the filters (see ADR-0013 keys
`ais.range_nm` / `ais.hide_anchored` / `ais.stale_s`):

- Compute bearing-from-own-ship and range using
  `navmath::greatCircleBearing` and `navmath::greatCircleDistance`.
- Draw a small triangle on the compass at the bearing, with a stroke
  thickness proportional to `1 / range` (closer = thicker).
- Above the boat-speed centre group, add a 3-line "nearest 3" panel
  with NAME · RNG · BRG.

**Without GPS fix — list.** Plain scrollable list (LVGL `lv_list` or
a tall flex column). Columns: NAME (left, ellipsised at 12 chars),
TYPE (3-letter, like the converter's), SOG, CPA-or-`—`. Sort by
last_seen descending so freshly-heard targets appear at the top.
Range/bearing columns are blank because we have no own-ship position.

### PGN page — unchanged

Keep the existing 19-tile honeycomb. The grey reserved slots
(ENG-T / OIL-T / HEEL / PITCH / ROT / RUD) stay as visual placeholders
for future hardware.

### Comm page — two-column rewrite

Mockup:

```
WiFi : STA  ap=lcd-2.1     GPS  : 3D fix  8 sats  age 2s
IP   : 192.168.4.2         Power: 4.18V batt  USB 5.04V (chrg)
RSSI : -42 dBm  ch 1       Up   : 1h 23m 47s
peers: 2
```

Column widths roughly equal; left column = `Network`, right column =
`Hardware`. Drops the legacy BLE notify counters that survive in
`comm_pg` today. TX's current `Primary` page content (GPS, AXP2101,
uptime) folds in here; TX no longer has a standalone Primary page.

LVGL: implement once in `src/Ui.cpp`'s `buildCommPage` /
`refreshComm`, then port the equivalent (Arduino_GFX-rendered) onto
TX in `src_tx/main.cpp`.

### Page count

RX is currently `kNumPages = 6`. New value: `4` (and the `#ifdef`
that switched between 5 and 6 in the BLE/WiFi backends becomes
unconditional 4).

TX is currently `kNumPages = 5`. New value: `4`.

The swipe state machine in both envs is already page-count agnostic
(`(current_page + step + kNumPages) % kNumPages`).

## Converter screen — layout unchanged

Apply only the three AIS filters from the global settings snapshot
(ADR-0013):

- `ais.range_nm`        — hide targets farther than N NM (with default 12 NM, but the converter currently defaults its list at infinite range; settings override at runtime)
- `ais.hide_anchored`   — drop NavStatus ∈ {1, 5, 6} when true
- `ais.stale_s`         — drop targets whose `last_seen` is older than N seconds

These filters short-circuit the existing 4-row list builder in
`src_converter/Ui.cpp`'s `refreshTargets()` (a single `passes_filters`
helper called before each row is emitted).

No graphical changes to the converter UI: boat icon, DAISY badge,
ship-type colour palette, header, footer all stay as they are.

## Settings UI — lives on iOS

See [IOS_APP.md](IOS_APP.md). No Settings page on the LCDs in v1.

## Implementation order

Suggested punch-list, smallest blocks first:

1. **AWA cone flip** on Main — single-file change, instantly visible.
   Bench-test by watching the cone rotate as the simulator wind angle
   moves; thick end should now poke through the outer ring's bow zone.
2. **Comm page two-column rewrite** on RX. New layout, new fields.
   Don't yet drop the BLE counters (they go away when the BLE env
   does, which is independent of this work).
3. **Drop Settings + Wind + Sim pages** from RX. Update `kNumPages`,
   `pages[]` index array, `case n` blocks. Verify swipe still cycles
   correctly.
4. **AIS page on RX** (list layout first, since GPS isn't yet wired —
   step-5 hardware is still pending). Use the AisTargetStore as the
   source; the store doesn't currently live on RX — fan-out from the
   converter's replay (ADR-0012) populates it via the existing PGN
   dispatch path. Verify by triggering a sim AIS injection from the
   converter and watching it land on RX's list.
5. **AIS page compass overlay** — gated on `hasGpsFix()`. Implements
   the GPS path; works on the bench by setting a sim lat/lon on RX so
   `hasGpsFix()` returns true.
6. **Converter AIS filters.** Wire the three filter keys through the
   `refreshTargets` short-circuit. Defaults bake in if no settings
   snapshot has arrived yet.
7. **TX pages, mirror of RX.** Port the four pages onto Arduino_GFX
   for TX. Same content, different renderer.

Steps 1–6 are doable independently. Step 7 depends on the others
landing first so the TX port has a stable target to copy.

## What this is not

- This brief does not implement ADR-0013 (HTTP server, heartbeat
  snapshot, NVS cache). Those are prerequisites for the Settings UI
  to *do anything*, but the page-layout work in steps 1–7 above is
  independent of them — the no-go-zone read happens whether or not
  the snapshot mechanism is in place (fallback to compile-time
  default).
- It does not touch the BLE envs. `[env:nmea2k_tx]` and
  `[env:waveshare_esp32s3_touch_lcd_21_ble]` stay as-is; only the
  `_wifi` envs evolve.
