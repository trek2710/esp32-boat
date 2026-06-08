# esp32-boat

A family of small, **independent, self-contained boat devices**. Each device
is one ESP32-based box that does a single job on its own — own sensors, own
display, own settings — and connects only to a companion **iOS app over BLE**.
No shared bus, no boat network required.

> **v2 (2026-06).** This reverses the original design — three ESP peers on a
> WiFi "virtual N2K bus" — which was abandoned in the pivot to independent BLE
> devices ([ADR-0016](docs/adr/0016-independent-devices-over-ble.md)). The full
> v1 design is recoverable at the `v1-wifi-bus-archive` tag; its docs are in
> [docs/archive/](docs/archive/).

## Device 1 — AIS-radar

A **Waveshare ESP32-S3-Touch-AMOLED-1.75-G** (onboard LC76G GNSS) + a **Wegmatt
dAISy 2+** AIS receiver, battery-powered:

```
   VHF antenna ─► dAISy 2+ ──UART(IO16)──► AMOLED device ──BLE──► iPhone (AisRadar app)
                                              │  - decode AIVDM
   LC76G GNSS ───────onboard──────────────────┤  - own position
                                              │  - radar on the AMOLED
                                              └  - threat colouring
```

It decodes AIS targets from the dAISy, takes own position from the LC76G (or
the phone's GPS), and draws the targets **radar-style** on its round AMOLED —
with a whole-screen threat colour (dark → green → yellow → red). The iOS app
mirrors the same radar over BLE.

**Status:** working end-to-end on the bench/phone. See
[docs/ROADMAP.md](docs/ROADMAP.md). Remaining hardware steps: solder the LC76G
`R15`/`R16` jumpers (real own GPS) and connect the VHF antenna (real targets).

## Repo layout

```
esp32-boat/
├── devices/
│   └── ais-radar/            device 1 — its own PlatformIO project
│       ├── platformio.ini
│       └── src/              main, Radar, Ble, glue
├── shared/                   salvaged, reusable across devices
│   ├── ais/                  AIVDM decode + AisTargetDecoder + target store
│   ├── display/              AMOLED panel + LVGL + TCA9554 + LC76G GPS + touch
│   └── ble/                  GATT wire contracts (AisRadarBle.h)
├── ios/                      AisRadar — SwiftUI BLE-central app
├── docs/                     CONTEXT.md (glossary), ROADMAP.md, adr/, hardware/
├── scripts/                  gen_ais_test.py, patch helpers
└── docs/archive/             v1 docs (superseded by ADR-0016)
```

## Build + flash (device)

Each device is its own PlatformIO project:

```bash
cd devices/ais-radar
pio run                                    # build
pio run -t upload --upload-port /dev/cu.usbmodemXXXX   # flash
```

The AMOLED's native USB-CDC needs the `--no-stub` + `115200` upload flags
already set in its `platformio.ini`.

## iOS app

`ios/` is the **AisRadar** SwiftUI app — a CoreBluetooth central that connects
to the `ais-radar` device, decodes its notify characteristics
([shared/ble/AisRadarBle.h](shared/ble/AisRadarBle.h)), and draws its own
radar with a tunable scale. Build:

```bash
cd ios
xcodegen generate
open AisRadar.xcodeproj      # run on a real iPhone — BLE needs hardware
```

## Bench testing

`scripts/gen_ais_test.py` generates verified Type-18 AIS sentences (correct
checksums, round-trip-decoded) placed around a centre point, labelled with the
threat colour each one triggers. Inject them via the dAISy `T` menu.

## Docs

- [docs/CONTEXT.md](docs/CONTEXT.md) — glossary / domain language.
- [docs/ROADMAP.md](docs/ROADMAP.md) — what's done + what's next.
- [docs/adr/](docs/adr/) — architecture decisions (ADR-0016 is the v2 pivot).
- [docs/hardware/ais_radar_wiring.html](docs/hardware/ais_radar_wiring.html) — device wiring + power.

## License

MIT — see [LICENSE](LICENSE).
