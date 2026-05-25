# CANboat-style JSON as the virtual-bus wire format

The virtual bus carries one CANboat-style JSON object per UDP multicast
packet, e.g.
`{"pgn": 129038, "src": 35, "fields": {...}}`. We considered raw N2K frames
(smallest, but iOS-side decoders are scarce) and SignalK (modern marine
standard, but the full-server implementation is heavy on ESP32-C6, and
SignalK abstracts away the N2K shape we want to replicate). Decoded JSON
sits between them: self-documenting, parsable on iOS with no dedicated
library, and round-trippable back to N2K frames by a Bridge ESP that needs
to re-emit on a real bus.
