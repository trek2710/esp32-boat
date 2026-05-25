# Bridge ESPs default-deny when forwarding virtual bus → real N2K bus

> **Superseded 2026-05-24 by [ADR-0011](0011-safety-boundary-at-wifi-admission.md).**
> Round 84 chose "no PGN-content filter" — the elected writer
> forwards every WiFi-originated PGN to the real bus. The safety
> surface moved up the stack to WiFi admission (WPA2 + MAC
> allow-list). The reasoning below is preserved as historical
> context.


The virtual bus is read+write through a Bridge ESP. Bus → virtual is
unconditional (observation only); virtual → bus is restricted to an
allow-list of PGNs. Default-allow would let any WiFi peer (including the
iOS app) drive autopilot or engine PGNs onto the bus — a safety boundary we
don't want to cross by accident. The allow-list starts with non-safety
PGNs (waypoint, MOB, user-supplied marks) and is extended deliberately when
a use case appears.
