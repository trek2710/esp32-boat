#pragma once

#include <stdint.h>

// AIS display filters, ported from the v1 converter's settings
// (ais.range_nm / ais.hide_anchored). Applied consistently by the radar,
// the threat assessment, and the BLE publish so all three agree. Defaults
// are hardcoded for now; they become iOS-tunable settings later.

namespace aisfilter {

constexpr double kRangeCapNm  = 24.0;   // hide targets beyond this range
constexpr bool   kHideAnchored = true;  // hide nav-status anchored/moored/aground

// True if a target should be hidden, given its nav status + range from own.
inline bool hidden(uint8_t navStatus, double rngNm) {
    if (kHideAnchored && (navStatus == 1 || navStatus == 5 || navStatus == 6)) {
        return true;   // 1=anchored, 5=moored, 6=aground
    }
    if (rngNm > kRangeCapNm) return true;
    return false;
}

}  // namespace aisfilter
