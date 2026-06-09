#include "DeviceSettings.h"

#include <Arduino.h>
#include <Preferences.h>

namespace devsettings {
namespace {
DeviceSettings g;
constexpr const char* kNs = "ais-radar";
}

const DeviceSettings& get() { return g; }

void load() {
    Preferences p;
    if (p.begin(kNs, /*readOnly=*/true)) {
        g.rangeCapNm   = p.getUChar("rng", g.rangeCapNm);
        g.hideAnchored = p.getUChar("anc", g.hideAnchored);
        p.end();
    }
    Serial.printf("[settings] rangeCap=%u NM hideAnchored=%u\n",
                  g.rangeCapNm, g.hideAnchored);
}

void set(uint8_t rangeCapNm, uint8_t hideAnchored) {
    if (rangeCapNm < 1) rangeCapNm = 1;
    g.rangeCapNm   = rangeCapNm;
    g.hideAnchored = hideAnchored ? 1 : 0;
    Preferences p;
    if (p.begin(kNs, /*readOnly=*/false)) {
        p.putUChar("rng", g.rangeCapNm);
        p.putUChar("anc", g.hideAnchored);
        p.end();
    }
    Serial.printf("[settings] updated rangeCap=%u NM hideAnchored=%u\n",
                  g.rangeCapNm, g.hideAnchored);
}

}  // namespace devsettings
