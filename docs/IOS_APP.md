# iOS companion app — design brief + status

This is the spec for the **iOS app**, a SwiftUI sub-project that joins
the virtual N2K bus as a full peer and serves as the canonical
settings UI for the boat instruments. The repo lives at
`../esp32-boat-ios/` (sibling to the firmware repo).

## Status (round 85, 2026-05-25)

**Shipped (v1.6 step 2–4):** xcodegen project, 4-tab SwiftUI shell, UDP
listener, heartbeat publisher with `kind:'ios-app'`/priority 80,
settings POST/GET against the AP's HTTP server, AIS list, per-channel
staleness sweep, derived TWA/TWS/VMG, iPhone GPS publisher (opt-in,
default off), Position cross-check with bold-orange warning at > 30 m
delta vs bus GPS, SOG/COG derived locally from GPS position deltas.

**Pending (v1.6 step 6):** AIS map view (own ship + targets +
5-minute forward-projection arrows), Diagnostics tab polish
(PGN-rate honeycomb, raw-log drawer). Map inputs are all on the
bus already — a 129026 wire-up was declined in favour of local
SOG/COG derivation (see ROADMAP v1.6 step 5).

## Role on the bus

The iOS app is a **full peer** on the virtual N2K bus, identical in
protocol shape to the three ESP peers (`lcd-gps`, `lcd-2.1`,
`nmea-converter`):

| Property | Value |
|---|---|
| `kBoardPeerName` | `ios-app` |
| `kBoardKindName` | `ios-app` |
| `kBoardPriority` | 80 (lower than TX=100; never becomes AP) |
| Transport | WiFi STA, joins `_wifi_nmea2k` with WPA2-PSK |
| Heartbeat | PGN 65500, 5-second cadence, same JSON shape |
| Reads | Every instrument PGN — full mirror of `BoatState` |
| Writes | Settings via HTTP POST to AP (ADR-0013); optional PGN 129025 GPS publish (opt-in, default off) |

Because the priority is 80 and iOS can't act as a softAP, iOS never
runs the AP role. The role-election protocol (ADR-0009) handles this
naturally — iOS always advertises `role:'STA'` in its heartbeat and
the existing tiebreak never selects it.

## Platform

Native Swift + SwiftUI, Xcode project, iOS 17+.

- Networking: `Network.framework` (`NWConnection`) for the UDP
  receive socket. `URLSession` for the HTTP write path.
- Persistence: `UserDefaults` for last-known settings snapshot and
  paired-WiFi metadata; `SwiftData` if a future "log" feature wants
  history.
- Background: foreground-only in v1. iOS aggressively suspends
  background sockets, and the boat use case is "phone is out, in
  hand"; pushing this into background mode is a separate hardening
  pass.
- Distribution: TestFlight → App Store. Bundle ID
  `com.koefoed.esp32boat` (placeholder).

A future Android app would be a separate native Kotlin project
sharing only the [VIRTUAL_BUS_WIRE.md](VIRTUAL_BUS_WIRE.md) contract
plus ADR-0013.

## Tab layout

Four tabs in a `TabView`, each a `NavigationStack`:

```
┌──────────────────────────────┐
│ <Tab content>                │
│                              │
│                              │
│                              │
└──────────────────────────────┘
│  Boat  │  AIS  │ Set │ Diag  │
└──────────────────────────────┘
```

### Tab 1: Boat (instrument dashboard)

Live mirror of the main instruments — port of the LCDs' Main page
plus more verbose readouts that the round display can't fit:

- Big compass widget (SwiftUI `Canvas`, same wedge / no-go-zone /
  AWA-cone treatment as RX's main page).
- TWA / TWS / AWA / AWS / TWD as a 2×2 grid below the compass.
- BSP / SOG / STW / Heading / Depth / Heel as a 2×3 grid.
- Position (lat/lon) at the bottom; tap to copy to clipboard.
- Pull-to-refresh forces re-subscribe to the AP (debug affordance).

No interactivity in v1 — read-only.

### Tab 2: AIS

Full target list (no row limit, unlike the LCDs' 4-row converter
screen). Columns: NAME · TYPE · RNG · BRG · SOG · CPA. Tap a row to
push a detail view showing:

- Full vessel name + MMSI + call sign
- Ship type + nav status (decoded to English)
- Live: SOG, COG, heading
- Last known position + last_seen age
- Apple Maps link with the position

Filters are global (ADR-0013 `ais.*` keys) — the AIS tab reflects
whatever the settings snapshot says. A bar at the top lets the user
*temporarily* widen the filter for this session (e.g. show moored
ships in a marina) without changing the global setting.

### Tab 3: Settings

The whole reason the app exists. Edits push to
`POST http://192.168.4.1/settings` and wait for `200`. The UI shows
the AP's current `settings_v` in the nav-bar title so the user knows
when their edit lands.

Sections:

- **Simulator** — toggle row per `sim.*` key. Disabled grey when no
  AP is reachable.
- **Display** — `ui.brightness` slider (0–100), `ui.idle_dim_after_s`
  picker (30s / 1m / 5m / 15m / never).
- **Navigation** — `nav.no_go_deg` stepper (20–60° in 5° steps).
- **AIS filters** — `ais.range_nm` slider (1–48 NM), `ais.hide_anchored`
  toggle, `ais.stale_s` picker (1m / 5m / 10m / 30m / 1h).
- **About** — app version, paired SSID, current AP peer name.

### Tab 4: Diagnostics

Read-only. The "look at the bus" view that today lives on the LCDs'
Comm + PGN pages.

- **Peers** — list rows for every peer in the AP's negotiator table:
  name, kind, priority, role, last-seen age. iOS's own entry at the
  top, dimmed.
- **WiFi** — SSID, BSSID of the AP we're on, channel, RSSI, this
  device's IP.
- **PGN rates** — same 19-tile honeycomb concept but rendered as a
  list (rate per PGN, sparkline). Wider than the LCD layout because
  there's no round-screen constraint.
- **Raw log** — toggle-on developer drawer that tails the most recent
  N JSON PDUs the app has received. Useful when debugging a new
  setting key or a malformed peer.

## Connection lifecycle

```
launch
  ├─ load cached settings snapshot from UserDefaults
  └─ start NWPathMonitor
       ├─ on path satisfied + WiFi SSID == _wifi_nmea2k
       │    open UDP listener on kBusPort
       │    start 5-s heartbeat publisher
       │    GET http://192.168.4.1/settings on first connect
       └─ on path lost or SSID changed
            close UDP listener
            stop heartbeat
            mark UI as "offline" (greys + last-known timestamp)
```

The app uses `WiFi association == bus join`. There is no "pair"
flow — the user joins the WiFi network from iOS Settings the normal
way (one-time, password from
[wifi_credentials.md](../../.claude/projects/...)) and the app
just lights up when the SSID is right.

## Things explicitly out of scope (v1)

- iOS publishing instrument PGNs back to the bus, beyond the opt-in
  phone-GPS source (PGN 129025) shipped in step 4. Any further
  iOS-sourced PGNs (e.g. phone heading/IMU) stay reserved for v2.
- iOS as the AP. Can't happen on iOS — no softAP API.
- Push notifications (AIS proximity alert, no-go-zone alarm, low
  battery). v2+.
- Per-peer settings overrides. ADR-0013 explicitly chose all-global;
  any future per-peer config would require schema rev.
- Background-mode operation. v1 is foreground-only.

## See also

- [ADR-0013 — Settings control plane](adr/0013-settings-control-plane.md)
- [ADR-0001 — WiFi not BLE](adr/0001-wifi-not-ble.md)
- [SCREEN_REDESIGN.md](SCREEN_REDESIGN.md) — the LCD changes that
  triggered the iOS app's existence
- [CONTEXT.md](CONTEXT.md) — glossary, including new `iOS-app peer`
  entry
- [VIRTUAL_BUS_WIRE.md](VIRTUAL_BUS_WIRE.md) — wire format the iOS
  app speaks
