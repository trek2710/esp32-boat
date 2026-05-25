# WiFi AP/STA role election among virtual-bus peers

**Date:** 2026-05-23. Supersedes ADR-0008's hardcoded "TX hosts the AP at
the bench stage" workaround.

## Context

ADR-0007 named the Bridge ESP (the `nmea_converter` firmware env in
this repo) as the long-term softAP host. ADR-0008 made TX the bench-
stage host because the converter wasn't yet on the bus. Round 81
proved that works.

But the deployment model is *ad-hoc cockpit instrument* (Q1b in the
grilling): the set of present devices varies per boot. Sometimes only
TX is plugged in. Sometimes TX+RX. Sometimes all three. With ADR-0008,
the bench loop falls apart the moment TX isn't first to boot, and the
iOS app has nothing to join when TX is absent. We need a runtime
election among whichever peers are present.

## Decision

Replace ADR-0008's fixed TX-as-AP with a WiFi-only role election
protocol, run by each ESP32 peer on boot and continuously thereafter.

### Roles

Three peer kinds, each with a fixed (compile-time) priority:

| Peer       | Priority | Rationale                                                 |
|------------|----------|-----------------------------------------------------------|
| RX         | 200      | Indoor display; most protected RF position; powered last to be moved |
| converter  | 150      | C6 has CPU headroom; powered when boat instruments are on |
| TX         | 100      | Often outdoor near GPS; least protected RF position       |
| iOS app    | (n/a)    | STA-only by construction; ineligible to be AP             |

Priority is a compile-time `constexpr uint8_t kVbusPriority` declared
per env in `include/VbusRole.h` (selected via `-DVBUS_BOARD_KIND=...`
build flag). Future iOS-app-driven priority override is a documented
TODO; this round ships with fixed compile-time values.

### Cold-boot election

Each peer, on boot:

1. **Priority-weighted backoff.** Wait `(255 - kVbusPriority) / 10 × 4 s`.
   With the table above:
   - RX (priority 200): waits 0 s (the formula gives ~22 s but RX clamps to 0)
   - converter (priority 150): waits 4 s
   - TX (priority 100): waits 8 s
   The intent is "higher priority scans first, lower priority gives the
   higher-priority peer a chance to become AP first."
2. **WiFi scan** for SSID `_wifi_nmea2k` for 2 s.
3. **If found** → become STA, `WiFi.begin()`, proceed to runtime loop.
4. **If not found** → become AP, `WiFi.softAP()` with WPA2-PSK + CCMP
   (per ADR-0008 inline workaround), proceed to runtime loop.

### Runtime heartbeat

Every 5 s, every peer publishes one **control PGN packet** onto the
multicast group `239.255.78.85:60001` using PGN `65500` (in the
N2K manufacturer-proprietary single-frame range, 65280–65535):

```json
{
  "pgn": 65500,
  "src": 255,
  "peer": "esp32-boat-rx",
  "fields": {
    "event":     "heartbeat",
    "kind":      "RX",
    "priority":  200,
    "role":      "AP",
    "ap_ip":     "192.168.4.1",
    "uptime_ms": 123456
  }
}
```

`event` is one of `heartbeat`, `takeover_announce`, `going_down`.
`role` is one of `AP`, `STA`, `electing`.

Every peer maintains a small **peer table** (last-seen timestamp +
last-reported role/priority) keyed by `peer` field. Pruned when a
peer's heartbeat goes silent for > 30 s.

### Re-election triggers (AP-died path)

A STA peer triggers re-election when both:
- WiFi `WL_DISCONNECTED` for > 10 s, OR
- 5 consecutive missed AP heartbeats (25 s) while WiFi still reports
  `WL_CONNECTED`

On re-election: drop the WiFi link, re-run cold-boot election from
step 1 (priority-weighted backoff + scan).

### Polite handoff (higher-priority peer arrives)

When the current AP sees a STA peer with `priority > my_priority` in
the peer table:

1. The arriving high-priority peer sends 3 × `takeover_announce`
   over 1.5 s.
2. The current (low-priority) AP receives `takeover_announce`,
   sends 1 × `going_down`, waits 500 ms.
3. Current AP calls `WiFi.softAPdisconnect(true)`, then
   `WiFi.mode(WIFI_STA)` + `WiFi.begin()` to join the new AP.
4. The arriving peer brings up its own softAP under the same SSID,
   starts publishing its own heartbeat with `role: "AP"`.
5. All other STAs see their AP go down, get DHCP from the new AP
   on its 192.168.4.x range, multicast resumes within ~3–5 s.

Total outage: ~5–10 s. Acceptable per ADR-0006 (30 s republish
covers the data gap).

### iOS app

iOS is a STA peer that:
- Joins `_wifi_nmea2k` (whichever ESP32 is currently AP serves it)
- Subscribes to the multicast group
- May publish PGNs subject to Bridge ESP allow-list (ADR-0004)
- Sends its own `heartbeat` advertising `kind: "iOS"`, `priority: 0`,
  `role: "STA"` so other peers know it's present
- Never becomes AP (iOS can't softAP arbitrary peers)
- May publish GPS PGN 129025/129026 if it observes no boat GPS for
  > 10 s (deferred to iOS-app implementation; ESP firmware doesn't
  care which peer publishes GPS)

## Consequences

Pro:
- Bench reality (b) just works: any subset of {TX, RX, converter}
  cold-boots and forms a network.
- Boat reality: highest-priority peer present always wins, with no
  human reconfig.
- Wire protocol stays JSON-on-multicast — no new sockets, no BLE.
- ADR-0008's bench-stage TX-as-host comment block is removed from
  source; election makes it automatic.

Con:
- More code than ADR-0008's hardcoded role: new `RoleNegotiator`
  module shared by all three envs, AP↔STA mode transitions on
  handoff, heartbeat publish/parse logic.
- ~3–10 s extra boot delay for lower-priority peers (acceptable; only
  visible at first start, not in steady state).
- Handoff causes a ~5–10 s WiFi outage — visible in the UI as
  honeycomb tiles dimming, then refreshing. The 30 s republish from
  ADR-0006 keeps the UI from blanking permanently.

## Deferred

- iOS-app-driven priority override (per Q8 in the grilling).
- BLE for boot-time discovery (rejected after Q6 grilling — WiFi-only
  is simpler and avoids the radio-coexistence airtime cost).
- Aggressive handoff scenarios (e.g., the high-priority peer disappears
  mid-handoff). Ship the happy path first per the user's "stable
  before tuning" directive.
- Synchronization of NVS-stored priority across peers (not needed
  until iOS-app-driven override lands).
