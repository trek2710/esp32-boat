# esp32-boat — glossary

This project is a family of **independent, self-contained devices**. Each
device is one ESP-based box that does a single boat-related job on its
own — own sensors, own display, own persisted settings — and can
optionally be observed and configured by an **iOS companion** over
Bluetooth Low Energy. There is no shared bus and no coordination between
devices; each is an island.

This reverses the v1 design (a WiFi "virtual bus" mirroring a boat's
NMEA 2000 network). The pivot and what it retires: **ADR-0016**. The
retired bus vocabulary is listed at the bottom so old code/commit
references still resolve.

## Language

**Device**:
A self-contained ESP box that performs one boat-related job standalone.
It owns its inputs (e.g. GPS, AIS), renders its own result on its own
display, and persists its own **device-local settings** in NVS so it
works with no phone present. A device may optionally connect to the
**iOS companion** over BLE. Devices do **not** talk to each other.
_Avoid_: "peer" (there is no bus to be a peer on), "node", "board" when
you mean the logical device (a device is one board today, but the term
is about its role, not its silicon).

**AIS-radar device**:
The first device. A Waveshare ESP32-S3-Touch-AMOLED-1.75-G (onboard
LC76G GNSS) wired to a Wegmatt Daisy 2+ AIS receiver. It decodes AIS
targets from the Daisy's serial feed, takes own position from the
LC76G, and draws the targets **radar-style** (own ship centred, targets
by range/bearing) on its AMOLED — when it has a GPS fix. Self-powered,
needs no boat connection.
_Avoid_: "converter" (the v1 C6 converter bridged AIS→N2K; this device
shows AIS, it doesn't bridge), "RX/TX" (those were bus roles).

**iOS companion**:
The iOS app, acting as a BLE **central** that connects to a device
(the device is the **peripheral**). It observes the device's live data
(AIS targets, own GPS, status) and is the **only** way to change a
device's settings. One companion observes one device at a time; pairing
multiple devices is a future concern.
_Avoid_: "iOS peer" / "iOS-app peer" (bus-era term), "hub" in a
networking sense (it is a viewer + the settings writer, not a relay).

**Device-local settings**:
A device's configuration (e.g. radar range, AIS filters, brightness),
stored in the device's NVS. The device runs on its stored settings
standalone and ships with sane defaults, so it works out of the box with
no phone ever paired. Settings can be **changed only from the iOS
companion** over BLE — there is exactly one writer, so no sync conflict,
and the device needs no on-screen settings UI (its touchscreen is for
radar interaction only).
_Avoid_: "control plane" (the v1 HTTP-on-AP mechanism is gone),
"settings snapshot" (no heartbeat carries it).

**N2K bridging** (future, per-device):
Any device *may* later gain an SN65HVD230 transceiver and emit its data
onto a real NMEA 2000 backbone — and read from it where it makes sense.
This is an optional add-on to a given device, not the system's purpose.
The v1 NMEA 0183-AIS → N2K encoders and the strict-`src` mirror rule
(ADR-0005) are kept dormant as reference for when this is wanted.
_Avoid_: treating N2K as the target (it is no longer); "transceiver
peer" (bus-era term).

## Comfort / sea state (planned feature)

Vocabulary for a planned **comfort** device/page. Pinned before
implementation. **Note:** the original design (ADR-0014) assumed the v1
WiFi bus and an iOS-republish step that no longer exist; under the
device model a comfort capability is just another self-contained device
(or a page on one) that BLEs to the companion. The *terms* below stand;
the *mechanism* in ADR-0014 needs reworking for BLE.

**Sea state**:
The wave field around the boat, expressed as a **wave height** + a
**wave period**. No sensor here measures it directly, so a sea state is
always an *estimate*, a *manual observation*, or a *forecast* — never
ground truth. Carries the two numbers only; the felt result is the
**comfort index**.

**Comfort index**:
The user-facing output: a band (calm / moderate / rough / dangerous)
derived from a sea state's *steepness* — wave height against wavelength,
wavelength from the period (deep water: L ≈ 1.56·T²). The "wave height
vs period" relationship is the point: the same height is comfortable at
long period and dangerous at short period.
_Avoid_: "comfort ratio" (Brewer's static hull constant — different
thing).

**Boat-response model**:
The learned filter relating the true sea state to the motion an IMU
actually feels. Under the device model it would live in the iOS
companion (per boat) and the device would apply or receive it over BLE
— not "republish onto a bus" as ADR-0014 originally said.

## Retired vocabulary (v1 WiFi-bus era — see ADR-0016)

These describe the abandoned shared-bus architecture. Listed so old
commits, code comments, and ADRs 0001–0015 still resolve. **None
describe current behaviour.**

- **Virtual bus** — the WiFi distribution of N2K-style PGN packets.
  Gone: devices are islands, no shared transport.
- **AP / role election / elected writer** — the dynamic WiFi access-
  point + priority handoff (ADR-0009/0010). Gone: no multi-peer network.
- **Peer / off-bus peer / transceiver peer** — members of the virtual
  bus. Gone: replaced by **device** (+ future per-device **N2K
  bridging**).
- **Mirror direction / virtual-to-real direction** — the v1 real-N2K ↔
  virtual-bus forwarding (ADR-0005). Dormant: relevant again only under
  future **N2K bridging**.
- **Control plane / settings snapshot** — HTTP-on-AP settings write +
  heartbeat-embedded settings fan-out (ADR-0013). Gone: replaced by
  **device-local settings** written over BLE.
- **AIS replay** — periodic re-emit of the AIS cache onto the bus for
  late joiners (ADR-0012). Reworked: the iOS companion reads the
  device's current AIS target store on BLE connect instead.
- **Standalone pod / gateway mode** — the ADR-0015 deployment split.
  Subsumed: "standalone" is now the whole model (every device is
  standalone); "gateway" is the future per-device **N2K bridging**.

## Flagged ambiguities (historical)

- "bus" meant both the physical N2K backbone and the WiFi side —
  resolved in v1 (virtual vs real). Now moot: there is no WiFi bus.
- "Bridge ESP" / "allow-list" / "proxy-write" / "multicast group" —
  all retired during v1 (ADR-0010/0011/0012).
- The entire v1 bus model is retired by ADR-0016; see the list above.
