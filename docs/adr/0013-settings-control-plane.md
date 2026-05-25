# Settings control plane: HTTP write, heartbeat snapshot

**Date:** 2026-05-24

## Context

Settings — sim channel mask, no-go-zone half-angle, display brightness,
AIS filter parameters — used to live as LVGL touch-controls on the RX
Settings page (no-go-zone) plus a static "Simulator" page that didn't
actually toggle anything. The planned iOS companion app changes the
shape of the problem:

- iOS is a full peer (own heartbeat, `kind='ios-app'`, priority 80) and
  the natural place to edit settings (real keyboard, room to render
  forms, no swipe gymnastics on a 2-inch round screen).
- Once iOS owns settings, the LCDs drop their Settings + Sim pages
  entirely. The two LCDs converge on a 4-page set: Main / AIS / PGN /
  Comm (see [SCREEN_REDESIGN.md](../SCREEN_REDESIGN.md)).
- Settings have to reach every ESP peer, not just the AP, so the
  channel-mask change made on iOS is visible to the sim publisher on
  RX **and** the PGN-honeycomb page on TX.

Two protocol axes need pinning:

1. **How does iOS write a setting?** New PGN over the virtual bus, or
   an out-of-band HTTP endpoint on the AP?
2. **How does the AP fan the change out to other STAs?** Inline in the
   heartbeat snapshot, a dedicated control PGN, or HTTP polling per STA?

A third related question — persistence — affects whether settings
survive a power cut or an AP-role handoff.

## Decision

### Write: HTTP POST from iOS to the AP

iOS issues `POST http://192.168.4.1/settings` carrying a JSON body of
the change(s):

```http
POST /settings HTTP/1.1
Host: 192.168.4.1
Content-Type: application/json

{ "sim.wind": false, "ais.range_nm": 8 }
```

The AP serializes incoming requests, applies them to its in-memory
settings map, bumps `settings_v` by 1, persists to NVS, and responds
`200` with the new version:

```json
{ "settings_v": 43, "applied": ["sim.wind", "ais.range_nm"] }
```

### Propagate: snapshot embedded in the PGN-65500 heartbeat

The AP's existing 5-second heartbeat gains two new fields:

```json
{
  "pgn": 65500,
  "src": 255,
  "peer": "lcd-2.1",
  "fields": {
    "event": "heartbeat",
    "kind": "lcd-2.1",
    "priority": 200,
    "role": "AP",
    "ap_ip": "192.168.4.1",
    "uptime_ms": 5023167,
    "settings_v": 43,
    "settings": {
      "sim.wind": false,
      "sim.gps": true,
      "sim.depth": true,
      "nav.no_go_deg": 35,
      "ais.range_nm": 8,
      "ais.hide_anchored": true,
      "ais.stale_s": 600,
      "ui.brightness": 80,
      "ui.idle_dim_after_s": 300
    }
  }
}
```

STAs compare incoming `settings_v` against their local cached version.
On bump, they replace local settings wholesale (the snapshot is
canonical), persist to NVS, and re-render any affected UI on the next
tick.

STAs that are not the AP **do not** include `settings_v` / `settings`
in their own heartbeats. Only the AP is the source of truth.

### Storage: NVS on every peer

Every ESP (AP and STA alike) caches the most recently received
settings snapshot in NVS (`Preferences` on Arduino-ESP32, the
equivalent on pioarduino for the C6). On cold boot:

1. Read `settings_v` + `settings` from NVS. If absent, use baked-in
   defaults with `settings_v = 0`.
2. If we end up AP, our cached snapshot becomes the canonical
   broadcast immediately — STAs adopt it within one heartbeat.
3. If we end up STA, our cached snapshot drives local behaviour until
   the AP's first heartbeat lands; then the higher of the two
   `settings_v` wins. (In practice the AP's is always higher because
   AP is whoever holds the freshest copy by election design.)

iOS can also `GET /settings` to fetch the AP's current snapshot for
display in the Settings tab.

### Scope: all settings are global

There is one settings map, applied identically to every peer. No
per-peer overrides. Brightness applies to both LCDs the same way; AIS
range cap applies to both LCDs and the converter the same way.

Per-peer config that genuinely diverges (e.g. peer name, board kind,
priority) stays in the build flags (`-DVBUS_BOARD_KIND=N`) and not in
the runtime settings map.

### Initial setting keys (v1)

| Key | Type | Default | Affects |
|---|---|---|---|
| `sim.wind`            | bool  | true | RX sim publisher |
| `sim.gps`             | bool  | true | RX sim publisher |
| `sim.heading`         | bool  | true | RX sim publisher |
| `sim.depth`           | bool  | true | RX sim publisher |
| `sim.sea_temp`        | bool  | true | RX sim publisher |
| `sim.air_temp`        | bool  | true | RX sim publisher |
| `nav.no_go_deg`       | u8    | 35   | RX main page (port/stbd black sector width) |
| `ais.range_nm`        | u8    | 12   | LCDs + converter AIS filter |
| `ais.hide_anchored`   | bool  | true | LCDs + converter AIS filter |
| `ais.stale_s`         | u16   | 600  | LCDs + converter AIS filter |
| `ui.brightness`       | u8    | 80   | LCDs backlight |
| `ui.idle_dim_after_s` | u16   | 300  | LCDs backlight idle dim |

Schema is open-ended: new keys can be added in any future version
without protocol churn (unknown keys are ignored by ESPs that don't
care about them).

## Consequences

Pro:

- iOS code stays clean: standard `URLRequest` + JSON. No UDP socket
  plumbing for the write path.
- Bus stays instrument-only on the read direction. ESPs don't need an
  HTTP client (read path is just parsing the existing heartbeat).
- One source of truth (the AP) eliminates write-write conflicts. iOS
  asks the AP, AP applies, snapshot fans out. There is no concurrent
  edit path.
- AP-role handoff degrades gracefully: the new AP's NVS already holds
  the last snapshot it received, so it can keep broadcasting the
  correct settings_v on its first heartbeat.
- Power loss recovery is trivial: NVS read at boot, broadcast wins on
  next heartbeat.

Con:

- AP runs an HTTP server now. Tiny code surface (`WebServer.h` on
  Arduino-ESP32 2.x, similar on pioarduino), but it is new code and
  needs to bind a port that doesn't collide with `kBusPort = 60001`.
  Port 80 is the obvious choice.
- The heartbeat blob grows. Today's heartbeat fits in ~150 bytes; the
  settings v1 keys add ~280 bytes. UDP MTU is plenty (~1472), and our
  `kRxBufSize = 512` on RX needs a one-character bump to be safe —
  see implementation notes.
- iOS only talks to the AP. If iOS happens to be associated when the
  AP role flips, iOS will lose its connection until it re-resolves
  `192.168.4.1` against the new AP. iOS Network framework handles
  this transparently with NWPathMonitor, but the user may see a brief
  "saving…" hang during an AP handoff.
- Versioning is whole-blob, not per-key. A revert of one key bumps
  `settings_v` and propagates the entire snapshot. Costs a few hundred
  bytes per change; not meaningful at 5 s cadence.

## Alternatives considered

- **Settings as a new PGN (`65501 ControlMessage`) on the bus.** Keeps
  the protocol uniform (one wire format), bypasses the AP's HTTP
  server. Rejected because iOS would have to open a UDP socket and
  speak JSON-over-UDP for what is conceptually a synchronous web form
  — and because the round-trip ack (settings_v bumped, applied) is
  natural in HTTP but awkward in fire-and-forget UDP.
- **Per-PGN settings (one PGN per setting category).** Rejected — adds
  protocol overhead and forces a schema update whenever a new setting
  is introduced.
- **STAs poll AP's HTTP endpoint every 5 s.** Rejected because every
  ESP would need an HTTP client and a polling timer, doubling network
  traffic for the AP→STA settings path. The heartbeat is already
  flowing at the right cadence; piggybacking the snapshot is free.
- **Per-peer settings overrides.** Rejected as YAGNI — the only real
  use case (per-LCD brightness) is marginal, and the cost is UI
  complexity in iOS plus a more complex wire format.

## Implementation notes

- `kRxBufSize` is currently 512 on RX (`src/NmeaBridge.cpp`). The new
  settings blob plus existing heartbeat fields fit comfortably; bump
  to 1024 if any future setting pushes it close to the limit.
- `RoleNegotiator::buildHeartbeatJson` doesn't yet know about settings.
  Either extend its signature to take a `const SettingsMap&`, or move
  settings-into-heartbeat assembly to a helper one level up in
  `NmeaBridge.cpp` / `src_tx/WifiPublisher.cpp` /
  `src_converter/WifiPublisher.cpp` that wraps the negotiator's output
  and appends the settings block. The latter avoids polluting the
  negotiator with app state.
- HTTP server only runs while role is AP. On AP→STA transition, the
  server is torn down; on STA→AP transition, it's spun up. iOS sees a
  connection error during the handoff and retries.
- Settings parser in each STA: cheap path, only ever reads from
  `g_rxBuf` in the drain task; no allocation. Apply functions per
  setting key live next to the consuming subsystem (e.g. `setNoGoDeg`
  near the Main page rendering, `setSimMask` next to
  `wifiSimAndPublish`).
- iOS uses `Codable` against a small Swift struct mirroring the keys
  table above.

## See also

- ADR-0009 (WiFi role election) — explains the AP role and handoff.
- ADR-0010 (unicast star + AP relay) — explains why the AP is the
  natural single source of truth.
- ADR-0011 (safety boundary at WiFi admission) — explains why HTTP on
  the AP isn't a new safety surface (anyone on the WiFi already has
  PGN-bus access; adding HTTP doesn't broaden the attack surface).
- ADR-0012 (AIS cache replay) — uses the same "AP-side state, push to
  STAs" pattern at a different cadence.
- [SCREEN_REDESIGN.md](../SCREEN_REDESIGN.md) — page changes that
  triggered this decision.
- [IOS_APP.md](../IOS_APP.md) — full iOS-app briefing.
