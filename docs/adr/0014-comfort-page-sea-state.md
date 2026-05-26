# Comfort page: motion-reconstructed sea state, user-selected source

**Date:** 2026-05-26

**Status:** Proposed — design pinned, implementation deferred. Captured
now so the language and wire implications are agreed before any code.

## Context

We want a **comfort page** on the instrument displays, inspired by the
cruisers'-forum "wave height vs period" rule of thumb: the same wave
height is benign as long-period swell and dangerous as short-period
chop, because what matters is **steepness** (height vs wavelength, and
deep-water wavelength ≈ 1.56·T²; waves break near steepness 1/7).

The hard constraint: **this boat has no wave sensor.** `Instruments`
carries no wave height/period or pitch/heave, NMEA 2000 delivers no
sea-state from any device we own, and the virtual bus has no internet.
So a comfort number's inputs must be manufactured, not read off the bus
like wind or depth.

What we *do* have, discovered but unused: a **QMI8658 6-axis IMU on
each LCD** — `0x6B` on the RX (cockpit 2.1" round, noted in
`src/Ui.cpp` since round 10) and one on the TX (AMOLED, in a
locker/engine bay). Two motion sensors, two very different locations.

This ADR pins how motion becomes comfort. Implementation is explicitly
deferred; see **Open questions** for what is *not* yet decided.

## Decision

### Comfort is steepness, computed from a "sea state"

A **sea state** is a (wave height, wave period) pair. The **comfort
index** is a band (calm / moderate / rough / dangerous) derived from
the active sea state's steepness. See the glossary in
[CONTEXT.md](../CONTEXT.md#comfort--sea-state-planned-feature).

### Three source types, one user-selected active source

A sea state comes from one of three origins — **measured**, **manual**,
**forecast** — and the user **explicitly selects** which is active.
The comfort index is computed from the active source alone; the others
render alongside for comparison and are never auto-blended. Because
*measured* is per-IMU (below), the concrete selector choices are four:
**cockpit-measured · locker-measured · manual · forecast** (default:
cockpit-measured — comfort is what the crew feels, and the crew is in
the cockpit).

### Measured = IMU reconstruction, boat-distorted, two locations, unfused

Each LCD reconstructs a sea state from its own IMU: tilt-compensate the
accelerometer with the gyro, band-pass to the wave band, integrate to
heave, and estimate height + dominant period. The boat's hull turns the
real sea into this motion, so the result is an *inference* about the
sea, knowingly distorted. The two LCDs produce **two** measured
estimates (cockpit, locker); they are shown **side by side and never
fused** — the spread between a cockpit and a locker is itself
information.

### Boat-response model: learned, in iOS profiles, republished to the bus

The distortion is captured by a **boat-response model** (a learned
transfer function, per IMU location). It lives in an iOS **boat
profile**, keyed by a **boat ID** the bus advertises in its heartbeat,
so the right profile loads automatically and "multiple boats in the
app" = multiple profiles. iOS does the learning (storage + compute),
applies each IMU's model to that board's published motion summary, and
**republishes the two corrected measured sea states onto the bus** so
every display agrees. With no phone present, each board falls back to a
shipped **default model** on its own summary.

### Manual and forecast live in iOS

**Manual** sea state is height + period typed into the iOS app.
**Forecast** sea state is pulled from **GFS** by iOS when it has
internet — a planning / comparison reference, explicitly not a live
reading of the water outside.

## Consequences

Pro:

- Honest about uncertainty: nothing claims to *measure* the sea. The
  user sees measured-inference, manual observation, and forecast as
  distinct, selectable things — and the cockpit/locker spread exposes
  how location-dependent "comfort" is.
- Reuses the existing patterns: motion summaries ride the virtual bus
  like any PGN; the boat-response model and selection ride the
  iOS-owned side; the corrected republish mirrors the ADR-0013
  "AP-side state, push to peers" shape but with iOS as the deriver.
- The idle QMI8658s finally earn their keep, and the cockpit IMU
  measures motion exactly where comfort is felt.

Con:

- **iOS becomes a deriving publisher.** Today iOS only publishes its
  own GPS; here it computes a value *from other peers' data* and pushes
  it back. This contradicts the CONTEXT note that AIS replay is "the
  only republish-like behaviour" — that line must be revisited when
  this ships.
- Reconstruction from a single strapped-down IMU is fragile (double-
  integration drift, the hull's own response). The "measured" numbers
  are estimates, not truth; the UI must not over-state them.
- New per-board DSP (heave reconstruction) and a new bus message
  (motion summary) — neither exists yet.
- Forecast is only as fresh as the last time iOS had internet; at sea
  it is a pre-departure download, not live.
- Per-boat learned models add an identity concept (**boat ID**) the
  bus does not have today.

## Alternatives considered

- **Manual entry as the only source.** Simplest (no hardware, no DSP),
  and was the recommended first cut. Rejected by choice — the user
  wants the boat to estimate sea state itself from its motion.
- **GFS/buoy forecast as the live source.** Rejected as the *primary*
  reading — it's a forecast, not what's outside now — but kept as a
  selectable comparison/planning source.
- **Felt-motion index instead of reconstruction** (RMS vertical
  acceleration → ISO-2631-style comfort, no wave reconstruction).
  More robust signal-processing, but doesn't honour the "wave height
  vs period" framing the user asked for. Rejected; reconstruction
  chosen with eyes open about its fragility.
- **Fuse the two IMUs into one estimate** (average, or exploit bow–
  stern separation). Rejected — the boxes' relative geometry isn't
  rigidly known, so most of the "fusion" gain is noise reduction, and
  it hides *where* comfort is measured. Show both instead.
- **Boat-response model on the boat (RX NVS), bus = boat identity.**
  Lets LCDs self-correct with no phone and needs no boat-ID concept,
  but caps learning at ESP storage/compute and drops multi-boat.
  Rejected in favour of iOS-owned models (+ default-model fallback).
- **Auto "best-available" blending of the three sources.** Rejected —
  hard to reason about and hard to explain on a small round display;
  the user selects instead.

## Open questions (not yet decided)

- Wave-band corner frequencies and the height/period estimator
  (zero-crossing vs spectral peak).
- The **motion summary** wire shape — a new PGN vs a field on an
  existing one; what it carries (Hs/Tp estimate? RMS accel? a few
  spectral bins?) and at what cadence.
- How **boat ID** is derived (AP/transceiver identity? user-assigned?).
- The **comfort band** thresholds (steepness → calm/moderate/rough/
  dangerous), and units shown (m vs ft, s).
- Which displays carry the comfort page, and its layout (it is a
  natural sibling to the RX radar page).
- GFS access: source/endpoint, how iOS knows position for the lookup,
  and the pre-departure caching model.
- The learning algorithm for the boat-response model and how a profile
  bootstraps from the default.

## See also

- [CONTEXT.md](../CONTEXT.md) — "Comfort / sea state" glossary section
  (sea state, sources, active source, comfort index, boat ID, boat
  profile, boat-response model).
- ADR-0013 (settings control plane) — the "iOS writes, AP/peers adopt"
  shape this builds on; the active-source selection is a natural new
  setting.
- [IOS_APP.md](../IOS_APP.md) — where the boat profiles, manual entry,
  GFS fetch, and source selector would live.
