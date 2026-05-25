# esp32-boat

A cockpit instrument display for the boat. **Three ESP32 peers + an iOS
companion app** share a WiFi-based virtual N2K bus.

```
                                                         WiFi (_wifi_nmea2k)
                                                       ┌─────────────────┐
                                                       │ unicast star    │
                                                       │ + AP-as-relay   │
                                                       └─────────────────┘
                                                                ▲
   NMEA 2000 backbone                                           │
        │                                                       │
   SN65HVD230  ──►  TX (lcd-gps, S3)  ───── STA ────────────────┤
                                                                │
   Daisy 2+ AIS ──► converter (C6) ──────── STA ────────────────┤
                                                                │
                    RX (lcd-2.1, S3)  ───── AP (priority 200) ──┤
                                                                │
                    iPhone app          ──── STA ───────────────┘
```

## Status (round 85, 2026-05-25)

**v1 + v1.5 + v1.5b + v1.6 step 1–4 are end-to-end working on the
bench in simulation.** Five-page LCD layout converged (Main / AIS /
PGN / Comm), AIS sim flowing across all peers, iOS companion app
shipped with: live instrument mirror, AIS list, settings POST,
diagnostics. iPhone GPS is opt-in and cross-checks against the
bus-published GPS (bold orange when > 30 m off).

The full architecture is documented in
[docs/CONTEXT.md](docs/CONTEXT.md); roadmap in
[docs/ROADMAP.md](docs/ROADMAP.md); design decisions in
[docs/adr/](docs/adr/) (superseded ones moved to
[docs/adr/archive/](docs/adr/archive/)).

What's **not** yet wired:

- Real boat connectivity — `SN65HVD230` to a live N2K backbone (v1.5
  step 5, hardware-blocked; see
  [docs/OPENPLOTTER_NMEA2000.md](docs/OPENPLOTTER_NMEA2000.md) for the
  OpenPlotter-as-source bring-up path).
- LC76G GPS on the AMOLED-1.75-G — `R15`/`R16` 0Ω jumpers aren't
  populated from the factory. Driver code is parked; soldering the
  jumpers will light it up via Serial1 with no code change.
- COG/SOG dispatch (PGN 129026) — prereq for the planned AIS map view
  in the iOS app.
- Distribution signing for the iOS app — currently free-tier signed,
  so the app needs upstream internet at launch for Apple cert checks.
  $99/yr Developer Program would resolve this.

## The four peers

| Peer | Board | Role on bus |
|---|---|---|
| **lcd-2.1** | [Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm) (480×480 round LVGL, touch) | AP (priority 200), always-on instrument display in cockpit, runs the simulator |
| **lcd-gps** | [Waveshare ESP32-S3-Touch-AMOLED-1.75-G](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) (466×466 AMOLED, swipe touch, LC76G GNSS) | STA (priority 100), wiring-locker board with the N2K transceiver |
| **nmea-converter** | [Waveshare ESP32-C6-LCD-1.47](https://www.waveshare.com/esp32-c6-lcd-1.47) (172×320, BOOT button) | STA (priority 150), decodes AIS sentences from a Daisy 2+ on UART1, also runs a 3-target AIS sim for bench testing |
| **ios-app** | iPhone (iOS 17+) | STA (priority 80), SwiftUI app — live instrument mirror, AIS list, Settings tab (HTTP POST to AP), Diagnostics |

The AP role is decided at runtime by a priority-weighted backoff
election; RX always wins in practice. See ADR-0009.

## Wire protocol

Per-PGN JSON over UDP — one packet per PGN, fan-out via AP:

```
{"pgn":130306,"src":255,"peer":"lcd-2.1",
 "fields":{"reference":"Apparent","windSpeed":3.1,"windAngle":0.42}}
```

Full spec: [docs/VIRTUAL_BUS_WIRE.md](docs/VIRTUAL_BUS_WIRE.md).
Why this and not multicast / BLE / a broker: ADRs 0001, 0010, 0011.

A small control plane runs in parallel on the AP (port 80, HTTP):

```
GET  /settings                  → { "settings_v": 42, "settings": {...} }
POST /settings { "ais.range_nm": 8 }
                                → { "settings_v": 43, "settings": {...} }
```

The new snapshot fans out to every STA via the next 5 s heartbeat (PGN
65500). Full design: [ADR-0013](docs/adr/0013-settings-control-plane.md).

A captive-portal stub also runs on the AP (DNS hairpin + iOS probe URL
handlers) so iOS classifies `_wifi_nmea2k` as a normal internet
network and doesn't silently switch to cellular.

## Build + flash

```bash
# All three firmware envs (WiFi variants):
pio run -e nmea2k_tx_wifi              -e waveshare_esp32s3_touch_lcd_21_wifi              -e nmea_converter

# MAC-safe flasher (won't flash the wrong firmware to the wrong board):
./scripts/flash.sh tx_wifi          # lcd-gps
./scripts/flash.sh rx_wifi          # lcd-2.1
./scripts/flash.sh converter        # nmea-converter
```

PlatformIO envs:

| Env | Board | Notes |
|---|---|---|
| `nmea2k_tx_wifi`                       | AMOLED-1.75-G (S3) | Current TX firmware |
| `waveshare_esp32s3_touch_lcd_21_wifi`  | LCD-2.1 (S3)       | Current RX firmware |
| `nmea_converter`                       | LCD-1.47 (C6)      | AIS bridge + WiFi peer |
| `nmea2k_tx`                            | AMOLED-1.75-G      | Legacy BLE TX (round 78, still buildable) |
| `waveshare_esp32s3_touch_lcd_21_ble`   | LCD-2.1            | Legacy BLE RX |
| `waveshare_esp32s3_touch_lcd_21_sim`   | LCD-2.1            | Pure-sim, no transport — UI bring-up only |

WiFi credentials default to SSID `_wifi_nmea2k`, password `showmetrust`.
Override by copying `include/wifi_credentials.example.h` to
`include/wifi_credentials.h`.

## iOS companion app

Lives in a separate repo: `../esp32-boat-ios/`. Build with `xcodegen`
+ Xcode 16+. Setup steps in
[docs/IOS_APP.md](docs/IOS_APP.md) and the iOS repo README.

## Wiring

[docs/WIRING.md](docs/WIRING.md) — full pinout per board.
[docs/BOM.md](docs/BOM.md) — bill of materials.

## Repo layout

```
esp32-boat/
├── platformio.ini              build envs (WiFi + legacy BLE)
├── src/                        RX firmware (lcd-2.1)
│   ├── main.cpp                ESP-IDF init + LVGL pump
│   ├── BoatState.{h,cpp}       thread-safe instrument snapshot
│   ├── NmeaBridge.{h,cpp}      WiFi role election + UDP drain + dispatch
│   │                           + AP-side HTTP server + DNS captive stub
│   ├── Ui.{h,cpp}              LVGL 4-page UI (Main / AIS / PGN / Comm)
│   ├── magnetic_variation.{h,cpp}
│   └── display/                ST77916 + CST820 + TCA9554 + ST7701 drivers
├── src_tx/                     TX firmware (lcd-gps)
│   ├── main.cpp                4-page LVGL UI + AXP2101 + CST9217
│   └── WifiPublisher.{h,cpp}   STA-side bus participant + role election
├── src_converter/              converter firmware (nmea-converter)
│   ├── main.cpp                AIVDM parser + AIS sim + WiFi heartbeats
│   ├── ais_decoder.{h,cpp}     NMEA 0183 → tN2kMsg
│   ├── AisTargetStore.h        in-RAM target cache (16 slots)
│   ├── WifiPublisher.{h,cpp}   STA-side bus participant
│   └── Ui.cpp                  Arduino_GFX single-page display
├── include/
│   ├── VirtualBusJson.h        shared JSON wire helpers
│   ├── RoleNegotiator.h        role-election state machine
│   ├── VbusRole.h              per-env priority + peer name constants
│   ├── Settings.h              shared settings store + NVS persistence
│   ├── BoatBle.h               legacy BLE PDU struct definitions
│   └── lv_conf.h               LVGL config
├── docs/                       overview + per-doc files
│   ├── CONTEXT.md              glossary, single source of truth
│   ├── ROADMAP.md              what's done + what's next
│   ├── adr/                    active ADRs (0001, 0002, 0005, 0007, 0009-0013)
│   └── adr/archive/            superseded ADRs (0003, 0004, 0006, 0008)
├── scripts/flash.sh            MAC-safe build + flash + monitor helper
└── binaries/                   pre-built RX .bin (legacy BLE only)
```

## License

MIT — see [LICENSE](LICENSE).
