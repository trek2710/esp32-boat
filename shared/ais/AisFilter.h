#pragma once

#include <stdint.h>

// AIS display filters, ported from the v1 converter's settings
// (ais.range_nm / ais.hide_anchored). Applied consistently by the radar,
// the threat assessment, and the BLE publish so all three agree. The
// thresholds are runtime settings (see the device's DeviceSettings),
// changeable from the iOS app.

namespace aisfilter {

// True if a target should be hidden, given its nav status + range from own.
inline bool hidden(uint8_t navStatus, double rngNm,
                   double rangeCapNm, bool hideAnchored) {
    if (hideAnchored && (navStatus == 1 || navStatus == 5 || navStatus == 6)) {
        return true;   // 1=anchored, 5=moored, 6=aground
    }
    if (rngNm > rangeCapNm) return true;
    return false;
}

}  // namespace aisfilter
