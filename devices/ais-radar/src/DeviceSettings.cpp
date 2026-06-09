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
        g.depthThreshM = p.getUChar("dth", g.depthThreshM);
        g.chartLayers  = p.getUChar("chl", g.chartLayers);
        g.testTargets  = p.getUChar("tst", g.testTargets);
        g.phoneGps     = p.getUChar("pgp", g.phoneGps);
        g.projMin      = p.getUChar("prj", g.projMin);
        p.end();
    }
    Serial.printf("[settings] rangeCap=%u anc=%u depth=%u layers=0x%02X test=%u phoneGps=%u proj=%u\n",
                  g.rangeCapNm, g.hideAnchored, g.depthThreshM, g.chartLayers,
                  g.testTargets, g.phoneGps, g.projMin);
}

void set(uint8_t rangeCapNm, uint8_t hideAnchored, uint8_t depthThreshM,
         uint8_t chartLayers, uint8_t testTargets, uint8_t phoneGps,
         uint8_t projMin) {
    if (rangeCapNm < 1) rangeCapNm = 1;
    if (projMin < 1) projMin = 1;
    g.rangeCapNm   = rangeCapNm;
    g.hideAnchored = hideAnchored ? 1 : 0;
    g.depthThreshM = depthThreshM;
    g.chartLayers  = chartLayers;
    g.testTargets  = testTargets ? 1 : 0;
    g.phoneGps     = phoneGps ? 1 : 0;
    g.projMin      = projMin;
    Preferences p;
    if (p.begin(kNs, /*readOnly=*/false)) {
        p.putUChar("rng", g.rangeCapNm);
        p.putUChar("anc", g.hideAnchored);
        p.putUChar("dth", g.depthThreshM);
        p.putUChar("chl", g.chartLayers);
        p.putUChar("tst", g.testTargets);
        p.putUChar("pgp", g.phoneGps);
        p.putUChar("prj", g.projMin);
        p.end();
    }
    Serial.printf("[settings] updated rangeCap=%u anc=%u depth=%u layers=0x%02X test=%u phoneGps=%u proj=%u\n",
                  g.rangeCapNm, g.hideAnchored, g.depthThreshM, g.chartLayers,
                  g.testTargets, g.phoneGps, g.projMin);
}

}  // namespace devsettings
