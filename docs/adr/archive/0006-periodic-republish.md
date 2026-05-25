# Each publisher periodically republishes its current state

> **Superseded 2026-05-24 by [ADR-0012](0012-ais-cache-replay.md).**
> Round 84 dropped generic per-publisher republish. The only
> remaining "republish-like" behaviour is AIS cache replay on the
> converter, which has a different rationale (RF static reports are
> 6-min cadence and only the converter holds AIS state). All other
> PGNs rely on their natural publish cadence — late joiners catch
> up within ≤1 s for high-rate data and may wait the publish
> interval for slow data.


Each publisher on the virtual bus re-emits the current state of everything
it owns on a ~30 s timer, even when no new data has arrived. This solves
the "late joiner sees nothing" problem (iOS app boot, ESP reconnect after
a WiFi drop) without an explicit snapshot / request handshake. Bandwidth
cost is negligible at boat scale. Alternative considered: snapshot-on-
request — works, but adds protocol surface area and retry logic for a
problem multicast solves naturally by just saying the truth more often.
