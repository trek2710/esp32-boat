# AisRadar — iOS app (v2, ADR-0016)

A SwiftUI **BLE-central** app for the `ais-radar` device. It scans for the
device, subscribes to its two notify characteristics, decodes own-ship +
AIS targets (wire contract: `../shared/ble/AisRadarBle.h`), and draws its
own north-up radar with a tunable / auto-fit scale.

This replaces the archived v1 WiFi-bus app (recoverable at tag
`v1-wifi-bus-archive-ios`).

## Build

```sh
cd ios
xcodegen generate        # regenerate AisRadar.xcodeproj (brew install xcodegen)
open AisRadar.xcodeproj  # then run on a real iPhone (BLE needs hardware)
```

`.xcodeproj` is gitignored — `project.yml` is canonical.

## Layout

- `Sources/AisRadar/BleCentral.swift` — CoreBluetooth central (scan / connect / subscribe).
- `Sources/AisRadar/RadarModel.swift` — decoded model + little-endian byte readers.
- `Sources/AisRadar/RadarView.swift` — the radar Canvas.
- `Sources/AisRadar/ContentView.swift` — radar + scale controls + status.

## Status

Connects, decodes `BleOwnShip` / `BleTarget`, draws the radar. **Pending:**
the settings write path (iOS → device), once the device exposes it.
