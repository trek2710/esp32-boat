#pragma once

#include <stdint.h>

// BLE GATT contract for the AIS-radar device (ADR-0016).
//
// The device is the BLE *peripheral*; the iOS app is the *central*. v1 is
// notify-only: the device streams own-ship state and AIS targets; the iOS
// app draws its own radar (and picks its own scale). A settings write path
// (iOS → device) is reserved for later.
//
// All multi-byte fields are little-endian, structs are packed, and each
// fits inside the default 23-byte ATT MTU so no negotiation is required.
// Lat/lon are 1e7 degrees; SOG/COG are 0.1 units; sentinels mark "n/a".

#define AISRADAR_BLE_NAME      "ais-radar"
#define AISRADAR_BLE_SVC_UUID  "a15a0001-7a11-4b3c-8d2e-0f1a2b3c4d5e"
#define AISRADAR_BLE_OWN_UUID  "a15a0002-7a11-4b3c-8d2e-0f1a2b3c4d5e"  // own ship (notify)
#define AISRADAR_BLE_TGT_UUID  "a15a0003-7a11-4b3c-8d2e-0f1a2b3c4d5e"  // one AIS target (notify)
#define AISRADAR_BLE_GPS_UUID  "a15a0004-7a11-4b3c-8d2e-0f1a2b3c4d5e"  // host (phone) GPS (write)
#define AISRADAR_BLE_SET_UUID  "a15a0005-7a11-4b3c-8d2e-0f1a2b3c4d5e"  // settings (read/write/notify)

// Own-ship state. Pushed on every publish cycle. lat/lon always carry the
// position the radar is centred on (a bench coord when there's no real fix);
// flags bit0 says whether it's a real GPS fix.
struct __attribute__((packed)) BleOwnShip {
    int32_t lat_e7;       // 1e7 deg
    int32_t lon_e7;
    int16_t cog_deg10;    // 0.1 deg true; INT16_MIN = n/a
    int16_t sog_kn10;     // 0.1 kn; INT16_MIN = n/a (own speed, for app CPA)
    uint8_t flags;        // bit0: real GPS fix (0 = bench); bits1-2: threat
                          //   0=none 1=safe 2=alert 3=danger (the device
                          //   computes it so the app shows the same colour)
    uint8_t targets;      // live target count
};
static_assert(sizeof(BleOwnShip) == 14, "BleOwnShip size");

// One AIS target. Each live target is notified once per publish cycle; the
// central keys by MMSI and expires entries on its own. With the name field
// this is 38 B, so the link needs an ATT MTU ≥ 41 (iOS negotiates ~185).
struct __attribute__((packed)) BleTarget {
    uint32_t mmsi;
    int32_t  lat_e7;       // INT32_MIN = no position
    int32_t  lon_e7;
    int16_t  sog_kn10;     // 0.1 kn; INT16_MIN = n/a
    int16_t  cog_deg10;    // 0.1 deg true; INT16_MIN = n/a
    uint8_t  ship_type;    // AIS typeOfShip (0 = unknown)
    uint8_t  age_s;        // seconds since last heard (clamped 255)
    char     name[20];     // NUL-padded AIS name; name[0]==0 if unknown
};
static_assert(sizeof(BleTarget) == 38, "BleTarget size");

// Host (iPhone) GPS, written by the central to the GPS characteristic so the
// device can use the phone's position + motion as own ship before the LC76G
// is soldered. INT*_MIN = n/a.
struct __attribute__((packed)) BleHostGps {
    int32_t lat_e7;
    int32_t lon_e7;
    int16_t cog_deg10;    // 0.1 deg true
    int16_t sog_kn10;     // 0.1 kn
};
static_assert(sizeof(BleHostGps) == 12, "BleHostGps size");

// Device settings — read by the app on connect (current values), written by
// the app to change them (the device persists to NVS), notified on change.
// chart_layers is a bitmask over the chart tile's layer ids (see below) so
// each overlay can be toggled; depth_thresh_m keeps the shallow-water shading
// threshold a runtime control (DRVAL1 < threshold ⇒ shaded shallow).
struct __attribute__((packed)) BleSettings {
    uint8_t range_cap_nm;     // hide AIS targets beyond this range
    uint8_t hide_anchored;    // 1 = hide anchored/moored/aground
    uint8_t depth_thresh_m;   // shallow-water shading threshold, metres
    uint8_t chart_layers;     // bit0 coastline,1 land,2 depth,3 depth-contour,
                              //   4 TSS,5 buoys,6 lights
    uint8_t test_targets;     // 1 = inject the three demo AIS targets
};
static_assert(sizeof(BleSettings) == 5, "BleSettings size");

// Chart overlay layer ids — the bit index in BleSettings.chart_layers and the
// layer byte in the .c93t tiles produced by tools/chart-transcode.
enum {
    CHART_COASTLINE = 0,
    CHART_LAND      = 1,
    CHART_DEPTH     = 2,   // DEPARE depth areas (carry DRVAL1)
    CHART_DEPCNT    = 3,   // depth contours
    CHART_TSS       = 4,
    CHART_BUOYS     = 5,
    CHART_LIGHTS    = 6,
};
