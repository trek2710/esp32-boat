#pragma once

#include <stdint.h>

// Device-local settings, persisted in NVS. The device runs on these
// standalone; the iOS app is the only thing that changes them (ADR-0016).

struct DeviceSettings {
    uint8_t rangeCapNm   = 24;   // hide AIS targets beyond this range (NM)
    uint8_t hideAnchored = 1;    // hide anchored/moored/aground targets
    uint8_t depthThreshM = 10;   // chart deep-water cutoff (m): shallower → blue bands
    uint8_t chartLayers  = 0x17; // overlay layers: coastline+land+depth+TSS
    uint8_t testTargets  = 1;    // inject the three demo AIS targets
};

namespace devsettings {

const DeviceSettings& get();
void load();                                  // from NVS (defaults on first boot)
void set(uint8_t rangeCapNm, uint8_t hideAnchored, uint8_t depthThreshM,
         uint8_t chartLayers, uint8_t testTargets);   // update + persist

}  // namespace devsettings
