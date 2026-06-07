# esp32-boat-ios

Native SwiftUI companion app for the `esp32-boat` firmware project. Joins
the virtual N2K bus as a full peer, mirrors instrument PGNs, and serves
as the canonical settings UI (the ESP boards don't have keyboards).

Design spec lives in the firmware repo:

- `../esp32-boat/docs/IOS_APP.md` — app architecture, tabs, peer role.
- `../esp32-boat/docs/adr/0013-settings-control-plane.md` — HTTP+heartbeat
  control plane this app drives.
- `../esp32-boat/docs/VIRTUAL_BUS_WIRE.md` — JSON wire format.

## What's here right now (v1.6 step 2)

```
Sources/EspBoat/
├── App/                EspBoatApp.swift + RootTabView.swift
├── Models/             Settings.swift, Instruments.swift
├── Networking/         BusConstants.swift, SettingsClient.swift, BusState.swift
└── Tabs/               BoatTab.swift, AisTab.swift, SettingsTab.swift, DiagnosticsTab.swift
```

The Settings tab is **live**: every toggle / stepper POSTs the matching
key to `http://192.168.4.1/settings`, the AP echoes back a bumped
snapshot, and the UI adopts it. Boat / AIS / Diagnostics tabs render
from `BusState` but the UDP listener that populates instrument data
isn't hooked up yet (next iteration — `BusState.startHeartbeat()` and
the receive socket are stubbed).

## First-time setup

```bash
# One-time, on the Mac:
brew install xcodegen

# Each time the source tree changes shape (new files, new targets):
xcodegen generate
open EspBoat.xcodeproj
```

In Xcode:

1. Select the `EspBoat` target → **Signing & Capabilities**.
2. Pick your Apple ID / development team.
3. Plug in an iPhone, select it as the run destination, ⌘R.
4. On the iPhone: **Settings → Wi-Fi → `_wifi_nmea2k`**
   (password: `showmetrust`). Stay in foreground.
5. Open the **Settings** tab in the app — should read the snapshot from
   the AP and let you toggle keys.

## Why xcodegen

Xcode `.xcodeproj` files are large and prone to merge conflicts on a
multi-author project. `xcodegen` makes them deterministic from
`project.yml` and lets us keep the project file out of git
(`.xcodeproj` is gitignored). Regenerate any time.

If you'd rather track `.xcodeproj` directly, delete `project.yml`,
remove the gitignore line for `*.xcodeproj`, and commit the project
file as usual. xcodegen is a convenience, not a hard dependency.

## Bundle ID

Placeholder: `com.koefoed.esp32boat`. Change in `project.yml` before
TestFlight / App Store distribution.

## Roadmap (firmware repo `docs/ROADMAP.md` v1.6)

- [x] **Step 1** — Settings control plane on the firmware (HTTP +
      heartbeat + NVS).
- [x] **Step 2** — Xcode project scaffold + Settings tab against the AP
      (this commit).
- [ ] **Step 3** — Boat tab: UDP listener, JSON dispatch, compass
      widget.
- [ ] **Step 4** — AIS tab: target list + detail push + session
      filter override.
- [ ] **Step 5** — Settings tab polish (already functional; needs an
      offline state, validation, presets).
- [ ] **Step 6** — Diagnostics tab: peers list, PGN-rate honeycomb,
      raw-log drawer.
