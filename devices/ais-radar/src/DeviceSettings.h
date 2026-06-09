#pragma once

#include <stdint.h>

// Device-local settings, persisted in NVS. The device runs on these
// standalone; the iOS app is the only thing that changes them (ADR-0016).

struct DeviceSettings {
    uint8_t rangeCapNm   = 24;   // hide AIS targets beyond this range (NM)
    uint8_t hideAnchored = 1;    // hide anchored/moored/aground targets
    uint8_t depthThreshM = 3;    // chart shallow-water shading threshold (m)
    uint8_t chartLayers  = 0x05; // chart overlay layer bitmask (coastline+depth)
};

namespace devsettings {

const DeviceSettings& get();
void load();                                  // from NVS (defaults on first boot)
void set(uint8_t rangeCapNm, uint8_t hideAnchored,
         uint8_t depthThreshM, uint8_t chartLayers);  // update + persist

}  // namespace devsettings
