# Bench-stage exception: esp32-boat TX hosts the softAP

> **Superseded 2026-05-23 by ADR-0009.** The hardcoded TX-hosts-AP
> arrangement was replaced by a runtime role election: every ESP32
> peer scans on boot, joins as STA if an AP is up, else becomes AP
> itself. Higher-priority peers (RX > converter > TX) take over with
> a polite handoff if they arrive after a lower-priority AP. See
> [ADR-0009](0009-wifi-role-election.md) for the full protocol and
> the round-83 implementation.

**Date:** 2026-05-22 (decision); 2026-05-23 (recorded after first
TX/RX implementation pass); 2026-05-23 (rewritten after the
NMEA-converter project was folded into this repo as the
`nmea_converter` firmware env — see round 82); superseded 2026-05-23
by ADR-0009.

ADR-0007 names the Bridge ESP (the `nmea_converter` env in this repo)
as the long-term softAP host. For the current bench stage we run the
TX board (`nmea2k_tx_wifi` env) as the host instead, and every other
peer — RX (`..._wifi`), converter, future iOS app — joins as a
station.

**Why:** the converter has no multicast publisher yet (architecture
documented in ADR-0001..0007; implementation deferred) and no
SN65HVD230 transceiver wired on the bench yet. The TX, by contrast,
already publishes simulated PGNs continuously and is the natural
first publisher to validate the wire format against. Hosting the AP
on whichever device boots first and is most reliably present keeps
the bench loop closing on two devices today, instead of waiting for
a third one.

**SoftAP credentials (shared across the deployment):**
- SSID: `_wifi_nmea2k`
- WPA2 password: `showmetrust`

Stored in `include/wifi_credentials.h` (gitignored); templated in
`include/wifi_credentials.example.h`. All three firmware envs include
the same header so the credentials are pinned in one place.

**Flip plan:** when the converter ships its multicast publisher AND
its transceiver is wired, swap roles — converter starts the softAP
under the same credentials, TX switches to station mode. The change
is one `WiFi.softAP(...)` ↔ `WiFi.begin(...)` swap on each device,
no protocol change. ADR-0007 then becomes the steady state again and
this ADR can be marked superseded.

**Out of scope:** any cross-device sequencing (who boots first, who
handles AP fall-back). Bench reality is two devices on the workbench;
production reality is one AP host and N stations on the boat LAN.
