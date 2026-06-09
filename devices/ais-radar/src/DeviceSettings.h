#pragma once

#include <stdint.h>

// Device-local settings, persisted in NVS. The device runs on these
// standalone; the iOS app is the only thing that changes them (ADR-0016).

struct DeviceSettings {
    uint8_t rangeCapNm   = 24;   // hide AIS targets beyond this range (NM)
    uint8_t hideAnchored = 1;    // hide anchored/moored/aground targets
};

namespace devsettings {

const DeviceSettings& get();
void load();                                  // from NVS (defaults on first boot)
void set(uint8_t rangeCapNm, uint8_t hideAnchored);  // update + persist

}  // namespace devsettings
