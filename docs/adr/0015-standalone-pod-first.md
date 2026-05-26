# Standalone pod is the first release target

**Date:** 2026-05-26

**Status:** Accepted.

## Context

The whole repo was built to **mirror a real NMEA 2000 backbone**: a
transceiver peer reads the boat's bus, fans PGNs onto the virtual WiFi
bus, and the LCDs render wind / depth / heading / boat-GPS. Every ADR so
far assumes that backbone exists.

The boat actually available for a first sea trial has **no NMEA 2000
network and no wind instrument** — and, on inspection, the kit has no
magnetic compass either (the RX I²C bus is touch / IO-expander / RTC /
QMI8658 IMU only — a 6-axis accel+gyro, no magnetometer). So:

- No wind, depth, STW, sea temp, or boat-GPS.
- No absolute heading — only **COG** from GPS, valid when moving.

What *is* available without any boat connection:

- **iPhone GPS** (CoreLocation), already published as PGN 129025.
- **AIS** via the **Daisy 2+** receiver over 3.3 V TTL serial into the
  converter — decoded and published onto the virtual bus with no N2K
  involved at all.
- The two LCDs' **IMUs** (motion / future comfort, deferred — ADR-0014).
- The **iOS app** (map, list, settings, GPS source).

Power is self-contained: the LCDs have batteries, the converter runs off
a powerbank. So a useful product exists *today* for a boat with zero
instruments — it just isn't the product the repo was aimed at.

## Decision

Make a **standalone pod** the first release ("sea-trial v1") and treat
boats-without-N2K as first-class. Four scoping decisions:

1. **Scope = AIS + GPS pod (LCD + iOS).** Real AIS via the Daisy +
   iPhone GPS, shown on the RX (radar + AIS list) and the iOS app (map
   + list). Comfort/IMU, wind/depth, a dedicated GPS module, and the
   N2K **gateway mode** are all deferred.

2. **Position = iPhone GPS only.** No GPS hardware in v1. The LCDs are
   therefore phone-dependent for own-position: with the app foregrounded
   and on the WiFi they centre on the real position; without it they
   show "NO GPS FIX". AIS targets still arrive without the phone — only
   the own-ship geometry (range/bearing, radar centre) needs it.

3. **RX page order unchanged for v1.** The wind-compass Main page still
   lands first and the radar is one swipe away. Reordering/cutting dead
   pages is deferred — not worth blocking a trial on UX.

4. **Field mode via a single `sim.master` switch.** One setting gates
   *all* simulator publishing (RX sim + converter AIS sim). Bench =
   master on (today's behaviour); trial = one tap off so only real
   sources flow. This exists specifically to stop `sim.gps` (Copenhagen
   harbour, 10 Hz) from beating the iPhone's real fix (1 Hz).

This **reverses the prior non-goal** "supporting boats without an NMEA
2000 backbone." The kit now has two **deployment modes** (see
CONTEXT.md): **gateway** (wired to a real backbone) and **standalone
pod** (no boat connection). Pod is the near-term target; gateway remains
supported.

## Consequences

Pro:

- There is a genuinely useful, testable product *now*, on the boat we
  actually have, with no new hardware — a portable AIS+GPS plotter.
- Forces the codebase to tolerate "no boat sensors", which it must do
  anyway; pod mode is the honest minimum.
- Small, well-bounded work list (see ROADMAP "Sea-trial v1").

Con:

- Own-position depends on a foregrounded phone — fragile, and the
  obvious fast-follow (a real GPS module on the converter's spare UART)
  is explicitly *not* in v1.
- No heading sensor means COG-only; displays must not imply a bow
  heading, and "which way am I pointing" at anchor is unavailable.
- The wind/depth/honeycomb pages render dead ("—") in pod mode and we
  chose not to hide them for v1 — a slightly rough first impression.
- A second deployment mode is now a permanent thing to keep working in
  both directions.

## Alternatives considered

- **Pod + IMU/comfort in v1.** Rejected — comfort is a large, research-y
  effort (ADR-0014) that would push a sea trial out by weeks.
- **iOS-only first release.** Rejected — the LCDs are the heart of the
  project and already mostly work; cutting them undersells what's ready.
- **Add a real GPS module for v1.** Rejected for v1 (hardware + driver +
  test delays the trial); kept as the top fast-follow because
  phone-only position is the pod's weakest point.
- **Pod build flag that compiles out dead pages.** Rejected for v1 —
  a new build variant to maintain; "leave as-is" is cheaper.
- **Toggle each `sim.*` off by hand / flip sim defaults to off.**
  Rejected — seven switches is error-prone (forgetting `sim.gps`
  teleports you), and flipping defaults inverts the bench workflow. A
  single master switch is one tap and hard to get wrong.

## Open questions / fast-follows

- A real GPS module (u-blox-class on the converter UART) to make the pod
  phone-independent — the most likely v1.1.
- A magnetometer add-on for true heading (would re-enable a real compass
  rose, heading-up radar, and wind-triangle math if wind ever appears).
- Comfort/IMU (ADR-0014) once the pod is proven.
- Pod-mode page-set cleanup (hide the dead wind/depth pages).

## See also

- ADR-0013 (settings control plane) — `sim.master` is one more key in it.
- ADR-0014 (comfort page) — the deferred IMU feature.
- [CONTEXT.md](../CONTEXT.md) — "Deployment modes" (gateway vs pod).
- [ROADMAP.md](../ROADMAP.md) — "Sea-trial v1" prioritised work list.
