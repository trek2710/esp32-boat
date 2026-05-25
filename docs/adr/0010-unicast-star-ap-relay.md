# Unicast star with AP-as-relay — supersedes ADR-0003

**Date:** 2026-05-24

## Context

ADR-0003 picked UDP multicast on `239.255.78.85:60001` as the virtual
bus transport. That choice held for rounds 80–83 but had a known
weakness — ESP32's softAP firmware does not deliver STA-originated
multicast to its own lwIP stack (see
`memory/sta_to_ap_multicast_limitation.md`). The bench coped by
placing the data publisher on the AP (round 83b), but every direction
involving a STA-as-publisher was effectively broken:

- iOS publishing GPS fallback / waypoints would never reach the AP.
- A non-AP transceiver peer mirroring real N2K traffic would never
  reach the AP.
- Even periodic heartbeats from STAs slipped through only sometimes,
  at low cadence, by luck.

The grilling session for round 84 (`docs/CONTEXT.md` history) chose to
replace the transport entirely rather than accept the limitation.

## Decision

Drop multicast. The virtual bus is now **unicast star with the AP as
the only relayer**:

1. Every peer is a WiFi STA or the AP (ADR-0007, ADR-0009). One peer
   is elected AP; the rest are STAs.
2. **Publish:** a STA wanting to publish a PGN unicasts the existing
   ADR-0002 JSON packet to the AP's IP (`192.168.4.1`) at UDP port
   `60001`. The AP also publishes (locally generated or received from
   its own physical backbone) by feeding the same packet into its
   own relay path below.
3. **Relay:** the AP receives every WiFi-side PGN at the UDP socket,
   enumerates its associated STAs from the softAP station table, and
   unicasts a copy to each station — **except the source IP** of the
   incoming packet (loop prevention; see below).
4. **Subscribe:** every peer (including the AP) treats every received
   PGN as data — the application layer dispatches by `pgn` field.

The JSON wire format (ADR-0002) is unchanged; only the transport flips
from "join multicast group" to "send to AP IP / receive from AP IP."

### Loop prevention

The AP skips the source IP of each incoming packet when fanning out.
Because the AP is the only relayer in this topology, no packet can
return to its originator and no infinite forwarding loop is possible.
**No application-level state is needed** — no message IDs, no
seen-set, no TTL. The "filter" is a single-line topology rule:
`for sta in stations: if sta.ip != incoming.src_ip: sendto(sta.ip)`.

This was a deliberate trade against alternatives (origin field +
LRU seen-set; TTL counter): we accepted that STAs never relay so the
single-relayer-by-design invariant gives us loop-free fanout for free.

### Transceiver-peer election (folded into this ADR)

When more than one **transceiver peer** is present on the *same*
physical N2K backbone, exactly one writes WiFi-originated PGNs to it,
to keep packets from appearing twice on the bus with two different
claimed `src` addresses.

- Each peer advertises its transceiver capability in the
  control-PGN heartbeat (`has_transceiver: true|false`).
- Among all peers currently advertising `has_transceiver=true`,
  the one with the lowest MAC address (as a 6-byte big-endian
  integer) is the **elected writer**. Non-elected transceiver peers
  silently swallow every virtual-to-real write.
- The **mirror direction** (real bus → virtual bus) is unaffected —
  every transceiver peer that physically observes a PGN forwards it
  to the virtual bus regardless of election. If two transceiver
  peers happen to see the same real-bus PGN, the AP will receive
  two copies (from two source IPs); the receiver application can
  dedup by `pgn`+`src`+`fields` content or just live with two
  identical broadcasts.

This is intentionally minimal — there is no real-bus identity beyond
the transceiver's MAC for election purposes; future deployments with
multiple physical backbones bridged by different peers would need a
"which backbone" axis added.

## Consequences

Pro:
- The STA→AP multicast bug is gone, by construction.
- iOS publishing works (iOS unicasts to AP IP just like an ESP STA).
- Loop prevention is a one-liner instead of a stateful protocol.
- AP failure stops the bus, but ADR-0009 election handles that —
  same behaviour as in the multicast model.

Con:
- AP fanout cost is O(N) per packet — at our scale (3–4 peers,
  ≤100 pkt/s) trivial; at scale (>20 peers) measurable.
- AP CPU now handles every WiFi-side PGN. The drain task plus an
  enumerate-and-send loop is still well within the ESP32-S3's
  budget but is the new bottleneck.
- The AP must know every STA's IP. softAP's DHCP / station table
  provides this; no application-level peer discovery needed.

## Deferred / out of scope

- Reliability: UDP is still lossy. We could add per-packet ACKs
  given unicast, but the natural cadence of most PGNs already
  provides redundancy. AIS catch-up is handled by ADR-0012.
- Multi-backbone topology — out of scope until two transceivers
  on different backbones is a real configuration.
- Reactive replay on STA-associate (a snapshot push from AP to
  newly-joined STA) — possible later optimisation; for now, normal
  publish cadence covers most data and ADR-0012 handles AIS.
