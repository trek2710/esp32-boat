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

// Own-ship state. Pushed on every publish cycle. lat/lon always carry the
// position the radar is centred on (a bench coord when there's no real fix);
// flags bit0 says whether it's a real GPS fix.
struct __attribute__((packed)) BleOwnShip {
    int32_t lat_e7;       // 1e7 deg
    int32_t lon_e7;
    int16_t cog_deg10;    // 0.1 deg true; INT16_MIN = n/a
    uint8_t flags;        // bit0: real GPS fix (0 = bench); bits1-2: threat
                          //   0=none 1=safe 2=alert 3=danger (the device
                          //   computes it so the app shows the same colour)
    uint8_t targets;      // live target count
};
static_assert(sizeof(BleOwnShip) == 12, "BleOwnShip size");

// One AIS target. Each live target is notified once per publish cycle; the
// central keys by MMSI and expires entries on its own.
struct __attribute__((packed)) BleTarget {
    uint32_t mmsi;
    int32_t  lat_e7;       // INT32_MIN = no position
    int32_t  lon_e7;
    int16_t  sog_kn10;     // 0.1 kn; INT16_MIN = n/a
    int16_t  cog_deg10;    // 0.1 deg true; INT16_MIN = n/a
    uint8_t  ship_type;    // AIS typeOfShip (0 = unknown)
    uint8_t  age_s;        // seconds since last heard (clamped 255)
};
static_assert(sizeof(BleTarget) == 18, "BleTarget size");
