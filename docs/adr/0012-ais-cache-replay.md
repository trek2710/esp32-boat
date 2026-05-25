# AIS-only cache replay on the converter — supersedes ADR-0006

**Date:** 2026-05-24

## Context

ADR-0006 said every publisher re-emits its full state every 30 s so
a late-joining peer reaches a populated state without an explicit
snapshot handshake. That made sense over multicast where the late
joiner had no way to be noticed.

Round 84 retired multicast (ADR-0010) and replaced it with unicast
star + AP-as-relay. In that model:

- Most PGNs publish at high enough natural cadence (10 Hz for GPS /
  wind / heading; 1 Hz for depth / STW) that a freshly-joined peer
  sees a populated value within ≤1 s.
- The exception is **AIS** — Class A and Class B *static* reports
  (vessel name, ship type, dimensions) only broadcast on RF every
  6 minutes per spec. A peer that joins in the wrong half of that
  window could wait minutes to see vessel names in the local AIS
  display.
- Only one peer in the deployment owns the AIS state — the
  `nmea_converter`, in its `AisTargetStore`. No other peer has
  data to "republish."

Round-84 grilling chose option (a): kill the generic ADR-0006
republish; do AIS-specific cache replay on the converter only.

## Decision

The `nmea_converter` peer periodically re-emits every entry in its
`AisTargetStore` onto the virtual bus, using the same PGN encoders
it uses when first receiving an AIVDM sentence from the Daisy 2+.

### Cadence

Every 30 s. The walker iterates the store, encodes each target's
known fields (MMSI, name, type, nav status, sog, cog, lat/lon, etc.)
into the relevant PGNs (129038 / 129039 / 129040 / 129794 / 129802 /
129809 / 129810 as applicable), and unicasts each to the AP via the
same path a freshly-received sentence would use.

### `last_seen_ms` is not touched

Replay is a distribution event, not a re-acquisition. The store's
`STALE_AFTER_MS` eviction (currently 5 minutes since last RF
reception) continues to operate against the genuine real-bus
last-seen timestamp. A target whose RF reports stopped 4 minutes
ago is still on its way to eviction at 5 minutes, even though its
PGNs are being replayed every 30 s.

### Scope: AIS only

No other peer republishes anything. Wind, GPS, heading, depth, STW,
sea/air temp — all rely on their natural cadence to populate a late
joiner. If a peer joins between two slow updates, it waits until
the next update naturally arrives.

This is a deliberate trade-off: simpler than a generic per-peer
republish mechanism, and the AIS case is the only one where the
natural cadence is genuinely too slow to bridge.

## Consequences

Pro:
- Matches the actual gap (AIS static is the only "slow enough to
  notice" data on the bus).
- Reuses the converter's existing PGN encoders and target store;
  the only new code is a 30 s timer + iterator.
- No new protocol surface — replayed PGNs look identical to freshly-
  received ones, and consumers don't need to know the difference.
- No state on consumer peers beyond what they already have.

Con:
- A late-joining peer that wants depth/temp/wind during a long
  period of stable values may wait the full publish interval to
  see a value at all. For the bench simulator (publishes every
  100 ms–2 s), this is invisible. For a real-boat sensor that
  publishes only on change, it could matter.
- If multiple converters ever existed (one per physical bus), they
  would each replay their own AIS store, potentially duplicating
  if the same AIS target was seen on two backbones. Not a current
  configuration.

## Alternatives considered

- **Generic per-peer republish, as in ADR-0006.** Kills the
  simplicity gain — most peers have nothing meaningful to
  republish and would emit redundant data.
- **AP-cached snapshot push on STA-associate.** The AP iterates a
  PGN cache and unicasts the last value of every PGN to the new
  STA the moment it joins. Technically optimal (zero wasted
  steady-state bandwidth) but introduces a new mechanism (AP
  needs a per-PGN cache, snapshot iterator, association event
  hook) for a small benefit. Reserved as a future optimisation if
  late-joiner UX becomes a complaint.

## Implementation notes

The AisTargetStore already maintains exactly the data the replay
needs. The walker is a periodic timer call from the converter's
main loop, around the existing 5 s heartbeat (`[hb]` line) cadence
group, just running on its own 30 s cadence.
