# Independent self-contained devices over BLE (abandon the WiFi virtual bus)

**Date:** 2026-06-07

**Status:** Accepted. **Supersedes** ADR-0001 (WiFi not BLE), ADR-0007
(softAP network model), ADR-0009 (WiFi role election), ADR-0010
(unicast star + AP relay), ADR-0011 (safety at WiFi admission),
ADR-0013 (settings control plane), ADR-0015 (standalone pod-first).
**Demotes** ADR-0002 (canboat JSON wire format), ADR-0005 (strict mirror
src), ADR-0012 (AIS cache replay) to dormant/reworked. **Revises**
ADR-0014 (comfort page) mechanism.

## Context

The whole project was built to **integrate into a boat's NMEA 2000
network**: read the backbone, mirror its PGNs onto a WiFi "virtual bus"
shared by several ESP peers, and render instruments. v1.5/v1.6 grew an
elaborate stack to make that bus work — role election, a unicast-star
relay through an elected AP, a WiFi-admission safety boundary, and an
HTTP settings control plane (ADRs 0007–0013).

Two facts collapsed that premise:

1. **There is no boat to integrate with.** The user has no N2K backbone
   to test against and no boat instruments (no wind, depth, heading).
   ADR-0015 already conceded this and pivoted to a "standalone pod," but
   kept the WiFi-bus machinery underneath.
2. **The WiFi virtual bus is too complicated to configure** for actual
   use — role election, softAP join, captive-portal stubs, AP handoff.
   It was heroic engineering for a problem the user no longer has.

What actually works and is worth keeping is much smaller: the
display/touch/PMIC bring-up for the boards (hard-won), the AIS AIVDM
**decode** (`src_converter/*.h`, pure logic), and the radar **geometry**.

## Decision

Rebuild the project as a family of **independent, self-contained
devices** (see [CONTEXT.md](../CONTEXT.md)):

1. **Each device is an island.** It does one boat job standalone — own
   inputs, own display, own NVS-persisted settings — and works with no
   phone and no other device. Devices never talk to each other.

2. **The only link is device → iOS companion over BLE.** The device is
   the BLE peripheral; the iOS app is the central. This **reverses
   ADR-0001**: we accept BLE's low bandwidth (AIS + GPS are low-rate, so
   it is ample) in exchange for escaping WiFi configuration entirely.

3. **Settings: device-persisted, iOS-written.** The device stores its
   settings in NVS and runs on them standalone with shipped defaults.
   They can be **changed only from the iOS companion** over BLE — one
   writer, no sync conflict, no on-device settings UI (the touchscreen
   is for radar interaction only).

4. **First device = the AIS-radar device.** The
   ESP32-S3-Touch-AMOLED-1.75-G (onboard LC76G GNSS) + a Daisy 2+ AIS
   receiver on a spare UART, drawing AIS targets radar-style on its
   AMOLED when it has a GPS fix. Milestone 1 includes the BLE link to a
   fresh iOS app.

5. **N2K becomes a future, optional, per-device add-on.** A device may
   later gain an SN65HVD230 to emit/read PGNs, but that is no longer the
   point of the system. The N2K encoders and the strict-`src` rule
   (ADR-0005) are kept dormant as reference.

6. **Start from scratch, preserve the driver knowledge.** Tag the
   current state (`v1-wifi-bus-archive`) so it stays recoverable, then
   rebuild a clean per-device source tree, extracting the AMOLED
   display/touch/PMIC/GPS init and the AIS-decode headers. Old ADRs are
   **superseded, not deleted** (this repo's convention).

## What each superseded/affected ADR becomes

| ADR | Was | Now |
|---|---|---|
| 0001 WiFi not BLE | chose WiFi over BLE | **superseded** — BLE, point-to-point to iOS |
| 0007 softAP network | the AP/softAP model | **superseded** — no WiFi |
| 0009 role election | dynamic AP election | **superseded** — no multi-peer network |
| 0010 unicast star relay | AP relays PGNs | **superseded** — no shared bus |
| 0011 WiFi admission safety | WPA2+MAC gate | **superseded** — BLE pairing is the boundary |
| 0013 settings control plane | HTTP-on-AP + heartbeat | **superseded** — device-local settings over BLE |
| 0015 pod-first | pod on top of WiFi bus | **superseded** — every device is standalone now |
| 0002 canboat JSON wire | JSON PGNs on the bus | **demoted** — BLE uses packed binary PDUs; JSON only if useful internally |
| 0005 strict mirror src | N2K bridging rule | **dormant** — relevant only under future N2K bridging |
| 0012 AIS cache replay | re-emit cache onto bus | **reworked** — iOS reads the device's target store on BLE connect |
| 0014 comfort page | bus + iOS-republish | **revised** — comfort is just another device/page over BLE |

## Consequences

Pro:

- The system finally matches reality: a useful box for a boat with no
  instruments and no network, testable on the hardware in hand.
- Massive reduction in moving parts — no role election, no relay, no
  AP handoff, no captive portal, no HTTP server. BLE pairing replaces
  all of WiFi onboarding.
- Each device is independently shippable and reasoned-about. Adding a
  second device (e.g. comfort) costs nothing in the first.

Con:

- A large amount of working v1 code is abandoned (the entire WiFi/bus
  stack, role negotiator, settings control plane, both the RX/TX WiFi
  publishers). Git-tagged, not lost, but no longer built.
- The radar must be **re-targeted** to the AMOLED. Both boards run
  LVGL 8.3.11 (the AMOLED uses Arduino_GFX only as LVGL's flush
  backend), so this is an LVGL→LVGL port — re-aim the canvas + screen
  dimensions, not a rewrite into a different graphics API.
- The iOS app is rebuilt from scratch (transport was the whole WiFi
  stack); the map/list/settings UI ideas carry, the code does not.
- BLE caps throughput; fine for AIS/GPS, but a constraint to respect if
  a future device wants high-rate data.
- Two boards from v1 (the RX round display and the C6 converter) have no
  role in milestone 1; their fate is deferred.

## Alternatives considered

- **Keep the WiFi bus, just simplify config (ADR-0015 pod).** Rejected
  — the user finds WiFi configuration itself the problem, not its
  surface area; BLE removes onboarding entirely.
- **One combined multi-board device over the bus.** Rejected — the
  goal is *independent* boxes, not a coordinated set.
- **Branch/fork for v2.** Rejected — same repo + a tag keeps history,
  memory, and docs in one place (see ADR-0016 decision 6).
- **Rewrite iOS transport but keep its code.** Considered; user chose a
  fresh iOS app for a clean break.

## Open questions / next

- Power: how the AMOLED device and the Daisy share a supply (Daisy is
  USB-powered; likely a shared powerbank / common 5 V).
- Spare-UART + pin budget on the AMOLED board for the Daisy feed
  alongside the LC76G UART, QSPI AMOLED, PMIC, and touch.
- The BLE GATT shape: characteristics for AIS targets, own GPS, status,
  and a settings write characteristic (crib the pattern from the
  dormant `include/BoatBle.h`, fresh content).
- Solder the LC76G UART jumpers (R15/R16) — prerequisite for own GPS.
- Fate of the RX round display and the C6 converter (future devices?).

## See also

- [CONTEXT.md](../CONTEXT.md) — the device / iOS-companion glossary.
- [ROADMAP.md](../ROADMAP.md) — the device-first roadmap.
- ADRs 0001–0015 — the superseded WiFi-bus design, kept for the trail.
- `include/BoatBle.h` — dormant BLE GATT pattern to crib from.
