# Strict-mirror src policy: off-bus peers don't claim N2K addresses

> **Partly amended 2026-05-24.** The Bridge-ESP / allow-list framing
> below is retired (ADR-0010 / ADR-0011). The `src` and `peer` field
> semantics described here are unchanged and still apply — read
> "Bridge ESP" as **transceiver peer** and "proxy-write direction" as
> **virtual-to-real direction**. There is no per-PGN allow-list
> gate any more; the elected writer (ADR-0010) emits every
> WiFi-originated PGN with its own claimed src.

In the mirror direction (real bus → virtual bus), PGNs are forwarded with
the original device's src preserved. In the proxy-write direction (virtual
bus → real bus), the Bridge ESP re-emits with its own claimed src; the
originating peer's identity is recorded in a `peer` field on the multicast
JSON but never appears on the real bus. We considered a "joint virtual
bus" model where every peer (including iOS) claims an N2K src, but that
creates conflict risk with real bus devices and forces every peer to
manage src state. The mirror model means only Bridge ESPs do address
claim, which keeps the address space coherent on each real bus.
