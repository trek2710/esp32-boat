# UDP multicast as the transport for the virtual bus

> **Superseded 2026-05-24 by [ADR-0010](0010-unicast-star-ap-relay.md).**
> Multicast was replaced by unicast star + AP-as-relay after round 83
> exposed that ESP32 softAP doesn't deliver STA-originated multicast
> to its own lwIP. The reasoning below is preserved as historical
> context; the current transport is unicast.


Peers join multicast group `239.255.78.85:60001` and both publish to and
subscribe from it. We considered MQTT (clean pub/sub semantics, but
introduces a broker SPOF and adds an MQTT client library to every peer) and
WebSocket (reliable streams, but point-to-point — N² connections in a
multi-peer setup). UDP multicast matches the bus-like nature of N2K — any
peer sends; all peers receive — with no central server. Lossiness is
acceptable because N2K traffic itself is lossy under arbitration, and the
republish policy (ADR-0006) bounds the time it takes a fresh subscriber to
reach a populated state.
