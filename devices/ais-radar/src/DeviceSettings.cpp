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
        g.depthThreshM = p.getUChar("dpt", g.depthThreshM);
        g.chartLayers  = p.getUChar("lyr", g.chartLayers);
        p.end();
    }
    Serial.printf("[settings] rangeCap=%u NM hideAnchored=%u depth=%u m layers=0x%02X\n",
                  g.rangeCapNm, g.hideAnchored, g.depthThreshM, g.chartLayers);
}

void set(uint8_t rangeCapNm, uint8_t hideAnchored,
         uint8_t depthThreshM, uint8_t chartLayers) {
    if (rangeCapNm < 1) rangeCapNm = 1;
    g.rangeCapNm   = rangeCapNm;
    g.hideAnchored = hideAnchored ? 1 : 0;
    g.depthThreshM = depthThreshM;
    g.chartLayers  = chartLayers;
    Preferences p;
    if (p.begin(kNs, /*readOnly=*/false)) {
        p.putUChar("rng", g.rangeCapNm);
        p.putUChar("anc", g.hideAnchored);
        p.putUChar("dpt", g.depthThreshM);
        p.putUChar("lyr", g.chartLayers);
        p.end();
    }
    Serial.printf("[settings] updated rangeCap=%u NM hideAnchored=%u depth=%u m layers=0x%02X\n",
                  g.rangeCapNm, g.hideAnchored, g.depthThreshM, g.chartLayers);
}

}  // namespace devsettings
