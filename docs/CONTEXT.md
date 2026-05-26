# esp32-boat — Virtual N2K bus glossary

This file pins the language used across the three firmware envs in this
repo (`nmea2k_tx_wifi`, `..._wifi`, `nmea_converter`) and the planned
iOS companion app.

The `nmea_converter` firmware decodes NMEA 0183-AIS sentences from a
Daisy 2+ receiver and (once its SN65HVD230 transceiver is wired) will
bridge between the **virtual bus** below and a physical N2K backbone.
The TX/RX boards are **off-bus peers** on the virtual bus.

## Language

**Virtual bus**:
The collective WiFi distribution of N2K-style PGN packets. Every peer
unicasts each PGN it publishes to the **AP**, which relays a copy via
unicast to every other peer. The resulting effect is "every peer sees
every PGN" — a wireless replica of the bus-like nature of N2K — without
a multicast group or broker.
_Avoid_: "the multicast group" (the multicast transport was retired in
ADR-0010); "network", "channel", "bus" (without "virtual").

**AP**:
The peer currently elected as WiFi access point under ADR-0009. Hosts
the softAP `_wifi_nmea2k`, accepts WiFi associations from the other
peers, and is the only relayer on the virtual bus. Every WiFi-side PGN
flows through it. If the AP disappears, the bus stops until a new AP
is elected.

**Transceiver peer**:
A peer that has an SN65HVD230 (or equivalent) wired to a physical N2K
backbone. Bridges packets between that backbone and the virtual bus in
both directions. Capability advertised in the peer's heartbeat
(`has_transceiver`) so other peers can compute the elected writer (see
**Elected writer**). Today only the `nmea_converter` env is intended to
be a transceiver peer.
_Avoid_: "Bridge ESP" (retired in ADR-0011 — the term was tied to the
allow-list safety gate, which is gone), "gateway", "master", "hub".

**Off-bus peer**:
A peer that has no physical N2K connection. Participates in the
virtual bus, can publish and observe, but cannot write to any real
N2K backbone. The iOS app is always an off-bus peer.
_Avoid_: "client", "station".

**Mirror direction**:
Real N2K bus → virtual bus. The transceiver peer forwards every
observed real-bus PGN onto the virtual bus, preserving the original
device's `src` so subscribers see the real-bus device address (per
ADR-0005).

**Virtual-to-real direction**:
Virtual bus → real N2K bus. The **elected writer** (see below) writes
every WiFi-originated PGN to its physical backbone, with its own
claimed `src` (per ADR-0005). There is no per-PGN filter (ADR-0011
moved the safety surface from PGN allow-listing to WiFi admission via
WPA2 + MAC allow-list).
_Avoid_: "proxy-write direction" (retired in ADR-0011 — the "proxy"
framing implied an allow-list gate that no longer exists).

**Elected writer**:
When more than one transceiver peer is present on the same physical
backbone, exactly one of them performs the virtual-to-real direction
to keep PGNs from appearing twice on the bus. Election is by lowest
MAC among the transceiver peers (ADR-0010); non-elected transceiver
peers continue to mirror real → virtual but silently swallow
virtual → real writes.

**AIS replay**:
The `nmea_converter` peer periodically (~30 s) re-emits every AIS
target it holds in its `AisTargetStore` onto the virtual bus, using
its existing PGN encoders. Lets a late-joining peer see vessel
names/types without waiting for the next 6-minute RF static report
(ADR-0012). Does NOT update `last_seen_ms` on the targets — eviction
of stale targets still works as normal. The only "republish-like"
behaviour on the bus; no other peer republishes anything (ADR-0006
was superseded).
_Avoid_: "republish" as a generic term — it now refers only to AIS
replay.

**Control plane** (round 85):
The HTTP path iOS uses to write settings: `POST /settings` against
the AP at `192.168.4.1:80`. Distinct from the **virtual bus** (which
carries instrument PGNs over UDP). Only the AP exposes the control
plane; ESPs in STA role do not. See ADR-0013.
_Avoid_: calling settings a "PGN" — they don't travel on the bus in
the write direction.

**Settings snapshot**:
The block of global settings the AP includes in every PGN-65500
heartbeat — `settings_v` (monotonic version) + `settings` (key/value
map). STAs compare incoming `settings_v` to their cached version and
adopt the snapshot on a bump. Defined in ADR-0013. The blob is
authoritative; whatever it says is the live setting on every peer
within ≤1 heartbeat (~5 s).
_Avoid_: "config push" (no push, it's piggybacked on the heartbeat
the AP would send anyway), "settings PGN" (there is no settings PGN
— settings are a *field* inside the existing heartbeat PGN).

**iOS-app peer**:
The iOS companion app participating as an off-bus peer
(`kind='ios-app'`, priority 80). Always STA, can never be AP. Reads
the bus, writes settings via the control plane. See [IOS_APP.md].

## Deployment modes

How the kit is used on a given boat. Most code paths must tolerate
**standalone pod** mode, where wind / depth / STW / boat-GPS simply
don't exist.

**Gateway mode**:
At least one **transceiver peer** is wired to a real N2K backbone; the
kit bridges that backbone onto the **virtual bus** (mirror direction)
and shows real instrument PGNs (wind, depth, heading, …). The original
target deployment.

**Standalone pod**:
No boat connection at all. A self-powered box (LCD batteries +
powerbank on the converter) that shows **iPhone GPS** position + **AIS**
(from the Daisy 2+ over serial, no N2K needed) + later IMU motion. The
boat may have no instruments, no N2K, and no wind. This is the first
**sea-trial** target; see [ROADMAP.md](ROADMAP.md).
_Avoid_: assuming a real N2K bus or any boat sensor exists — in pod
mode the only live inputs are iPhone GPS, the Daisy AIS feed, and the
onboard IMUs.

## Comfort / sea state (planned feature)

Vocabulary for the planned **comfort page**. Nothing here is built yet;
this pins the language before implementation. See ADR-0014.

**Sea state**:
The wave field around the boat, expressed as a **wave height** + a
**wave period**. No sensor you own measures this directly, so a sea
state is always an *estimate*, a *manual observation*, or a *forecast*
— never ground truth on this boat. Carries the two numbers only; the
felt result is the **comfort index**.
_Avoid_: "the waves" as if measured; "sea state" to mean a single
scalar (it is always height **and** period).

**Sea-state source**:
One of three origins for a sea state's height+period pair —
**measured**, **manual**, or **forecast**. Exactly one is the **active
source** at any time.

- **Measured sea state**: reconstructed from an onboard IMU's motion.
  There are **two**, one per LCD location (cockpit / locker); they are
  shown side by side and **never fused** — a cockpit and a locker
  genuinely move differently, and the spread is information. Each is
  distorted by the boat's own response to the waves (see **boat-response
  model**), so it is an inference about the sea, not the sea itself.
- **Manual sea state**: height + period entered by hand in the iOS app.
- **Forecast sea state**: pulled from **GFS** by the iOS app when it has
  internet. A planning / comparison reference, explicitly *not* a live
  reading of the water outside right now.

**Active source**:
The sea-state source currently selected (by the user) to drive the
comfort index. Because **measured** is per-location, the concrete
choices are four: **cockpit-measured**, **locker-measured**, **manual**,
**forecast** (default: cockpit-measured — the felt comfort where the
crew sits). Selection is explicit — the comfort index is computed from
the active source alone; the others are shown for comparison but never
silently blended in.
_Avoid_: "best available" / "auto" — there is no automatic fusion;
the user picks.

**Comfort index**:
The user-facing output of the comfort page: a band (e.g. calm /
moderate / rough / dangerous) derived from the **active source**'s
*steepness* — wave height against wavelength, where wavelength comes
from the period (deep water: L ≈ 1.56·T²). The "wave height vs period"
relationship is the whole point: the same height is comfortable at long
period and dangerous at short period.
_Avoid_: "comfort ratio" (that is Brewer's static hull constant — a
different thing); "sea-state index" (the index is about *felt comfort*,
derived from the sea state, not the sea state itself).

**Boat ID**:
A stable identifier for the physical vessel, advertised by the bus in
its heartbeat. Lets the iOS app auto-select the right **boat profile**
when it joins. Distinct from a **peer**'s name/`kind` (those identify a
box on the bus; the boat ID identifies the hull all the boxes are
bolted to).

**Boat profile** (iOS):
The per-boat record the iOS app keeps, keyed by **boat ID** — its name
plus its learned **boat-response model**. "Multiple boats in the app"
means multiple profiles; switching boats loads a different model.

**Boat-response model**:
The learned filter relating the true **sea state** to the motion the
IMUs actually feel — the boat's hull turns waves into motion, and this
model is the inverse used to estimate the sea back out. Lives in the
**boat profile** (iOS does the learning), and is **per IMU location** —
the cockpit and locker mountings get their own models. A coarse default
ships so an un-learned boat — or one with no phone present — still
yields a rough **measured sea state**.
_Avoid_: "calibration" (it is a learned transfer function, not a
one-shot zero/offset); "RAO" in user-facing text (fine in code/ADR).

**Measured sea state — who computes it**:
Each LCD reconstructs a low-rate *motion summary* from its own IMU and
publishes that on the bus. The iOS app applies that IMU's
**boat-response model** to each summary and **republishes both
corrected measured sea states** onto the bus, where they are shown side
by side (not fused). With no phone present, each board falls back to the
default model on its own summary. (Note: this makes the iOS app a *deriving* publisher — revisit
the AIS-replay glossary line that calls AIS replay "the only
republish-like behaviour" once this ships.)

## Relationships

- A **transceiver peer** operates in both the **mirror direction**
  (real → virtual) and, if it is the **elected writer**, the
  **virtual-to-real direction**.
- An **off-bus peer** is publish-and-observe-only on the virtual bus.
- The **iOS app** is always an off-bus peer.
- The **AP** is whichever peer the role-election picks (ADR-0009) —
  could be a transceiver peer or an off-bus peer. AP role and
  transceiver-peer role are independent.
- **AIS replay** runs only on the `nmea_converter`, since it is the
  only peer that maintains an AIS target cache.

## Example dialogue

> **Dev:** "If the iOS app pushes a waypoint to the virtual bus, what
> does a real N2K display see?"
> **Designer:** "A waypoint PGN whose `src` is the elected writer's
> claimed N2K address. The iOS app's identity is on the JSON `peer`
> field but not on the real-bus wire. The waypoint reaches the
> backbone because there's no per-PGN filter — the safety surface is
> who's allowed to *join* the WiFi (WPA2 + MAC allow-list), not what
> they can publish once joined."

## Flagged ambiguities

- "bus" initially meant both the physical N2K backbone and the WiFi
  side — resolved: the WiFi side is the **virtual bus**; the physical
  side is the **real N2K bus**.
- "publisher/subscriber" was used early with broker semantics —
  resolved: there is no broker. The AP is a relayer, not a broker.
- "multicast group" was the virtual bus until round 84 — retired in
  favour of the unicast-star + AP-relay model in ADR-0010. The phrase
  "multicast" should no longer appear when describing current
  behaviour.
- "Bridge ESP" / "allow-list" / "proxy-write" / "republish" — all
  retired in round 84 (ADR-0010, 0011, 0012). Replaced as documented
  above.
- "settings PGN" / "control PGN" — there is no such PGN. Settings
  write travels over HTTP (control plane); settings read travels in a
  snapshot field on the existing heartbeat PGN (ADR-0013, round 85).
