# Safety boundary moves to WiFi admission — supersedes ADR-0004

**Date:** 2026-05-24

## Context

ADR-0004 made the **Bridge ESP** the per-PGN safety gate: any
WiFi-originated PGN had to be on the bridge's allow-list before it
would be written to the physical N2K bus. The allow-list defaulted to
deny for safety-critical PGNs (engine, autopilot), specifically to
prevent a buggy or compromised peer (including a future iOS app) from
driving those PGNs onto a real backbone.

That model is no longer viable in round 84's architecture. The
"Bridge ESP" concept was retired (ADR-0010 — there's no longer a
designated gatekeeper, just transceiver peers that happen to have a
backbone wired). And the user explicitly chose the "no filtering"
direction in the round-84 grilling (option (c) of the safety
question): every PGN flowing on the virtual bus reaches the real
backbone unmodified, full stop.

## Decision

There is no per-PGN filter on the virtual-to-real direction. The
elected writer (ADR-0010) writes every WiFi-originated PGN to its
backbone, including engine, autopilot, and any other PGN regardless
of safety classification. The safety surface moves up the stack
to **WiFi admission**:

- The softAP is locked with WPA2-PSK (existing — ADR-0008's CCMP
  fix still applies).
- A **MAC allow-list** at the WiFi-admission layer restricts which
  devices can even associate to the AP. Only allow-listed MACs can
  join; non-allow-listed devices are rejected at the WiFi-layer
  authentication.
- Devices that successfully join are trusted to publish whatever
  they want onto the virtual bus, and the elected writer will
  forward to the real bus without inspection.

The MAC allow-list mechanism itself is **deferred** — neither where
the list lives (NVS on each peer, distributed via mesh, hardcoded at
compile time) nor how new devices are added is decided. The
deferred-decisions list in `memory/MEMORY.md` carries this forward.

## Consequences

Pro:
- Matches the user's mental model ("full mesh, everything everywhere
  all the time, no filtering").
- Far simpler implementation — no per-PGN routing tables, no
  per-bridge policy state.
- Future iOS-app GPS fallback, waypoint pushes, and any other peer
  capability "just work" without an allow-list ceremony.

Con:
- The boundary now relies entirely on WPA2 password secrecy + the
  MAC allow-list. A user who shares the WiFi password with a
  curious-but-untrustworthy guest, or runs the boat on a router
  that doesn't enforce a MAC allow-list, has no fallback.
- A compromised peer (any of the ESPs) gets full bus write access.
  In ADR-0004 the bridge would still refuse to write critical PGNs;
  now nothing refuses.
- The "no allow-list" stance is **harder to reverse than the
  reverse direction** — once consumers depend on being able to
  publish arbitrary PGNs, retro-fitting filters means breaking those
  features.

The user accepted these trade-offs in the round-84 grilling. The
rationale: in the deployment scenarios envisaged (Q1b: ad-hoc
cockpit instrument world; not a publicly-accessible service), the
WiFi admission surface is far smaller than the PGN-class surface.
Boats don't have anonymous internet-facing WiFi.

## Deferred

- MAC allow-list mechanism: where the list lives (NVS,
  build-time, runtime-distributed); how new devices are added; how
  devices are removed; how the list is synced across peers if a
  peer hosts the AP and only it enforces admission. Tracked in
  `memory/MEMORY.md`.
- Whether to add a structural filter for **proprietary-range PGNs**
  (65280–65535) on the virtual-to-real direction — the user
  confirmed in the round-84 grilling that the heartbeat PGN
  (65500) and other proprietary-range traffic should NOT be written
  to the real bus. That's a small implementation rule
  (`if (pgn >= 65280 && pgn <= 65535) skip;`), not a
  user-configurable policy.
- Whether to expose any of this in a UI for runtime tuning.

## Things that still apply from ADR-0005

ADR-0005 (strict-mirror `src`) is amended, not retired — the
**`src` field semantics survive intact**:
- Mirror direction (real → virtual): preserve original device's src.
- Virtual-to-real direction (virtual → real): the elected writer
  claims its own N2K src for emission. The originating peer's
  identity is recorded in the `peer` field of the JSON packet
  (visible on the virtual bus, never on the real bus wire).

What ADR-0005 used to say about "the Bridge ESP gates by allow-list
when re-emitting" is gone with the allow-list itself. Only the
src-claim semantics remain.
