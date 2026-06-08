# Archived documentation (v1 — WiFi virtual bus era)

These docs describe the **v1** design — three ESP32 peers sharing a WiFi
"virtual N2K bus", an LCD instrument UI, and a WiFi iOS app. That whole
architecture was abandoned in the **v2 pivot** to independent, self-contained
BLE devices ([ADR-0016](../adr/0016-independent-devices-over-ble.md)).

They're kept for the reasoning trail; **none describe how the project works
today**. The full v1 source is recoverable at the `v1-wifi-bus-archive` tag.

| File | Was |
|---|---|
| `IOS_APP.md` | The v1 WiFi-bus iOS app spec (4 tabs, UDP, HTTP settings). v2 app: `../../ios/`. |
| `VIRTUAL_BUS_WIRE.md` | The canboat-style JSON wire format on the WiFi bus. v2 uses packed BLE structs (`shared/ble/`). |
| `SCREEN_REDESIGN.md` | The v1 LCD 4-page redesign (Main / AIS / PGN / Comm). |
| `OPENPLOTTER_NMEA2000.md` | Bring-up notes for feeding the v1 converter from an OpenPlotter/Signal K N2K source. |
| `WIRING.md` | v1 RX/TX board wiring. v2 device wiring: `../hardware/ais_radar_wiring.html`. |
| `NAVIGATION_MATH.md` | v1 instrument derivations (TWA/TWS/VMG, wind triangle) — not used by the AIS-radar device. |

Current docs live in the parent `docs/` directory: `CONTEXT.md` (glossary),
`ROADMAP.md`, the active `adr/`, and `hardware/ais_radar_wiring.html`.
