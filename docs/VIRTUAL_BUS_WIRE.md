# Virtual N2K Bus — Wire Specification

This document is the contract between the three firmware envs in this
repo — the TX board (`nmea2k_tx_wifi`), the RX board (`..._wifi`),
the AIS converter (`nmea_converter`, which decodes AIS today and will
bridge to a physical N2K bus once its SN65HVD230 is wired) — and a
planned **iOS companion app**.

Authoritative architectural decisions live in `docs/adr/0001-0008.md`.
This file pins the implementation details those ADRs leave open:
exact JSON shape, PGN numbers, field names, units, cadences, and
credentials. The converter env must match this document; the TX is
the first-mover publisher.

## Transport

- **Group:** `239.255.78.85` (administratively-scoped IPv4 multicast).
- **Port:** `60001` (UDP).
- **TTL:** `1` (boat-LAN scope; do not let multicast leak past an AP).
- **MTU budget:** stay under 1024 bytes per packet. One PGN per packet.

## Network

- SoftAP `_wifi_nmea2k` / WPA2-PSK + CCMP-only / `showmetrust`. See
  `~/.claude/projects/-Users-jeppekoefoed-Documents-Claude-Projects-esp32-boat/memory/wifi_credentials.md`.
- Role election (round 83 / ADR-0009): every ESP32 peer runs the same
  protocol on boot. Priority-weighted backoff (RX:0s, converter:4s,
  TX:8s), then a 2 s WiFi scan. If SSID is up → join as STA; if not →
  become AP. A higher-priority peer that arrives later takes over via
  polite handoff (`takeover_announce` + `going_down`, ~5–10 s outage).

## Packet shape

One JSON object per UDP packet, no framing, no terminator. UTF-8.

```json
{
  "pgn": 130306,
  "src": 255,
  "peer": "esp32-boat-tx",
  "fields": {
    "sid": 7,
    "windSpeed": 5.34,
    "windAngle": 2.117,
    "reference": "Apparent"
  }
}
```

- `pgn` *(uint16, required)* — the N2K PGN number this packet represents.
- `src` *(uint8, required)* — always `255` for off-bus peers (the N2K
  "unclaimed/null device" sentinel). Bridge ESPs in the mirror direction
  pass the real device's claimed src through here. See ADR-0005.
- `peer` *(string, required)* — application-level identity of the
  publisher: `"esp32-boat-tx"`, `"esp32-boat-rx"`, `"nmea-converter"`,
  `"ios-app"`. Subscribers may dedupe or filter on this.
- `fields` *(object, required)* — per-PGN payload, naming and units
  per the CANboat JSON convention.

### Unit conventions (CANboat-aligned)

- **Angles:** radians (degrees × π / 180). Range `-π…π` for relative,
  `0…2π` for compass-style absolute. The receiver converts to degrees
  for display.
- **Speeds:** metres per second. (1 knot = 0.514444 m/s.)
- **Distances:** metres.
- **Temperatures:** Kelvin. (°C + 273.15.)
- **Lat/lon:** decimal degrees as IEEE-754 double. ±90 / ±180.
- **Strings used for enums** (e.g. `"True"` / `"Magnetic"` /
  `"Apparent"`) — never raw integer codes.

### SID

The `sid` field is the per-PGN sequence ID that ties together
related PGNs (e.g. position-rapid + COG/SOG-rapid sharing the same SID
mean they describe the same instant). It's a `uint8` that wraps at
253; values 254/255 are CANboat "null". Publishers increment per
*publish event*, not per PGN — so one wind publish that emits a True
and an Apparent packet uses the same SID for both.

## PGN map (esp32-boat publish set)

Mapping from the existing `include/BoatBle.h` PDUs into individual
PGN packets. Each row is one UDP packet; the TX may emit several rows
per simulator tick.

### Wind — from `WindPdu`

| Bits set in `valid_mask`           | PGN     | fields                                                                                 |
|------------------------------------|---------|----------------------------------------------------------------------------------------|
| bit 1 (TWS) AND bit 0 (TWA)        | 130306  | `sid`, `windSpeed` (m/s), `windAngle` (rad, relative), `reference: "True"`             |
| bit 1 (TWS) AND bit 2 (TWD)        | 130306  | `sid`, `windSpeed` (m/s), `windAngle` (rad, compass abs), `reference: "True (ground referenced to North)"` |
| bit 4 (AWS) AND bit 3 (AWA)        | 130306  | `sid`, `windSpeed` (m/s), `windAngle` (rad, relative), `reference: "Apparent"`         |

Note: if both TWA and TWD are present, emit both — they're distinct
true-wind references.

### GPS — from `GpsPdu`

| Bits set                         | PGN    | fields                                                                            |
|----------------------------------|--------|-----------------------------------------------------------------------------------|
| bit 0 (LAT) AND bit 1 (LON)      | 129025 | `latitude` (deg), `longitude` (deg)                                               |
| bit 2 (COG) AND bit 3 (SOG)      | 129026 | `sid`, `cogReference: "True"`, `cog` (rad), `sog` (m/s)                           |

### Heading + boat speed — from `HeadingPdu`

| Bits set        | PGN    | fields                                                                                 |
|-----------------|--------|----------------------------------------------------------------------------------------|
| bit 0 (HDG)     | 127250 | `sid`, `heading` (rad), `reference: "Magnetic"`                                        |
| bit 1 (BSPD)    | 128259 | `sid`, `speedWaterReferenced` (m/s), `speedWaterReferencedType: "Paddle wheel"`        |

(Heading reference defaults to `Magnetic` because that's what the
simulator and most compasses produce. If a true-heading source ever
joins, it gets a separate row with `reference: "True"`.)

### Depth + temperatures — from `DepthTempPdu`

| Bits set        | PGN    | fields                                                                                       |
|-----------------|--------|----------------------------------------------------------------------------------------------|
| bit 0 (DEP)     | 128267 | `sid`, `depth` (m), `offset: 0.0`                                                            |
| bit 1 (AIR-T)   | 130316 | `sid`, `instance: 0`, `source: "Outside Temperature"`, `actualTemperature` (K)               |
| bit 2 (SEA-T)   | 130316 | `sid`, `instance: 1`, `source: "Sea Temperature"`, `actualTemperature` (K)                   |

### Attitude + engine — from `AttitudePdu`

| Bits set                                  | PGN    | fields                                                                |
|-------------------------------------------|--------|-----------------------------------------------------------------------|
| bit 0 (HEEL) AND/OR bit 1 (PITCH)         | 127257 | `sid`, `yaw: null`, `pitch` (rad), `roll` (rad)                       |
| bit 2 (ROT)                               | 127251 | `sid`, `rate` (rad/s)                                                 |
| bit 3 (RUD)                               | 127245 | `sid`, `instance: 0`, `position` (rad), `directionOrder: "No Order"`  |
| bit 4 (ENG-T)                             | 127489 | `sid`, `instance: 0`, `engineCoolantTemperature` (K)                  |
| bit 5 (OIL-T)                             | 127489 | `sid`, `instance: 0`, `engineOilTemperature` (K)                      |

Missing values: send `null` (JSON literal) rather than the field, so
subscribers can tell "unknown" from "zero". CANboat does this for
fields a PGN doesn't carry; we follow suit.

## Cadences

The TX publishes at the existing BLE cadences (which match N2K spec
rates) — see `src_tx/main.cpp` Step-7 globals for the canonical
numbers. Summary:

| PDU         | Publish period |
|-------------|----------------|
| Wind        | 100 ms (10 Hz) |
| GPS         | 100 ms         |
| Heading     | 100 ms         |
| Depth       | 1 s            |
| Sea temp    | 2 s            |
| Air temp    | 2 s            |
| Attitude    | 100 ms         |

Plus a **30-second republish** per channel: even if no new sample
arrived, re-emit the last known value. This is the late-joiner
catch-up mechanism from ADR-0006. The republish reuses the most
recent SID rather than minting a new one — same data, same SID.

## Receive side

The RX joins the multicast group, parses each packet, dispatches by
`pgn`, and calls the matching `BoatState` setter. PGN handlers are a
small switch in `src/NmeaBridge.cpp` (the `DATA_SOURCE_WIFI` branch),
mirroring what the production `tNMEA2000` path does on a real bus.

Lossiness is acceptable — UDP plus the 30 s republish bounds time to
populated state. The honeycomb's "dim" colour is the correct visual
response to a brief multicast drop; no special-case logic required.

## Versioning

If a PGN field name or unit ever changes, bump the multicast port
(`60001` → `60002`). Old subscribers stop seeing traffic (clean
failure) rather than silently misinterpreting fields.

## Control PGN — heartbeat / role election

PGN 65500 (N2K manufacturer-proprietary single-frame range) carries
the role-election control messages. Same wire format as data PGNs,
emitted by every peer every 5 s on the multicast group:

```json
{
  "pgn": 65500,
  "src": 255,
  "peer": "esp32-boat-rx",
  "fields": {
    "event":     "heartbeat",          // or takeover_announce / going_down
    "kind":      "RX",                 // TX / RX / converter / iOS
    "priority":  200,
    "role":      "AP",                 // AP / STA / electing
    "ap_ip":     "192.168.4.1",
    "uptime_ms": 123456
  }
}
```

Subscribers track a peer table (peer name → kind/priority/role/last-
seen) and use it to drive election + handoff decisions. See
`docs/adr/0009-wifi-role-election.md` for the full protocol.

## Cross-firmware obligations

The `nmea_converter` env (round 83 — partial):
- ✅ Participates in role election via PGN 65500 heartbeats.
- ⏳ AIS PGN multicast publish (129038/129039/etc.) — deferred to a
  follow-up round. Requires intercepting `tNMEA2000::SendMsg()` via
  a subclass of `NMEA2000_esp32_twai`.
- ✅ Honours the same 30 s republish rule for state it owns.

The iOS app must:
- Be subscribe-only initially. Publishing requires the proxy-write
  allow-list path on a Bridge ESP, which doesn't exist yet.
