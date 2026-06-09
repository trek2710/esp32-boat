#include "Radar.h"

#include <Arduino.h>
#include <lvgl.h>
#include <cmath>
#include <AisFilter.h>
#include <AisRadarBle.h>
#include "DeviceSettings.h"
#include "ChartOverlay.h"

namespace radar {
namespace {

constexpr int kW = 466, kH = 466;
constexpr int kCx = kW / 2, kCy = kH / 2;
constexpr int kR  = 205;
constexpr double kD2R = 0.017453292519943295;
constexpr uint32_t kDimAfterMs = 5000;
constexpr double kProjHr = 300.0 / 3600.0;

// Threat thresholds (tunable).
constexpr double kAlertSpeedKn     = 15.0;     // yellow if SOG above this …
constexpr double kAlertSpeedRangeM = 5000.0;   // … and within 5 km
constexpr double kAlertNearM       = 200.0;    // yellow if moving within 200 m
constexpr double kDangerCpaM       = 185.0;    // red if closest approach < ~0.1 NM …
constexpr double kDangerTcpaS      = 360.0;    // … within 6 min

enum class Threat : uint8_t { None = 0, Safe = 1, Alert = 2, Danger = 3 };

lv_obj_t*   g_canvas = nullptr;
lv_color_t* g_buf    = nullptr;

void rangeBearing(double lat1, double lon1, double lat2, double lon2,
                  double* rng_nm, double* brg_deg) {
    constexpr double kEarthNm = 3440.065, r2d = 57.29577951308232;
    const double phi1 = lat1 * kD2R, phi2 = lat2 * kD2R;
    const double dphi = (lat2 - lat1) * kD2R, dlam = (lon2 - lon1) * kD2R;
    const double a = std::sin(dphi / 2) * std::sin(dphi / 2)
                   + std::cos(phi1) * std::cos(phi2)
                   * std::sin(dlam / 2) * std::sin(dlam / 2);
    *rng_nm = kEarthNm * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    const double y = std::sin(dlam) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2)
                   - std::sin(phi1) * std::cos(phi2) * std::cos(dlam);
    double b = std::atan2(y, x) * r2d;
    if (b < 0) b += 360;
    *brg_deg = b;
}

// Threat level for one target, given own state (ownSog ≤ 0 ⇒ own stationary).
Threat assess(const AisTarget& t, double ownLat, double ownLon,
              double ownCogDeg, double ownSogKn, double rngNm) {
    const double rangeM = rngNm * 1852.0;
    Threat level = Threat::Safe;

    if ((t.sog_kn > kAlertSpeedKn && rangeM < kAlertSpeedRangeM) ||
        (t.sog_kn > 0.2 && rangeM < kAlertNearM)) {
        level = Threat::Alert;
    }

    // CPA collision test (needs both courses).
    if (t.sog_kn >= 0 && t.cog_deg >= 0) {
        const double midLat = ownLat * kD2R;
        const double e = (t.lon_deg - ownLon) * kD2R * 6371000.0 * std::cos(midLat);
        const double n = (t.lat_deg - ownLat) * kD2R * 6371000.0;
        const double kn2ms = 0.514444;
        const bool ownMoving = ownSogKn > 0.2 && ownCogDeg >= 0;
        const double oVe = ownMoving ? ownSogKn * kn2ms * std::sin(ownCogDeg * kD2R) : 0;
        const double oVn = ownMoving ? ownSogKn * kn2ms * std::cos(ownCogDeg * kD2R) : 0;
        const double tVe = t.sog_kn * kn2ms * std::sin(t.cog_deg * kD2R);
        const double tVn = t.sog_kn * kn2ms * std::cos(t.cog_deg * kD2R);
        const double rve = tVe - oVe, rvn = tVn - oVn;
        const double vv = rve * rve + rvn * rvn;
        if (vv > 1e-6) {
            const double tcpa = -(e * rve + n * rvn) / vv;
            if (tcpa > 0 && tcpa < kDangerTcpaS) {
                const double ce = e + rve * tcpa, cn = n + rvn * tcpa;
                if (std::sqrt(ce * ce + cn * cn) < kDangerCpaM) level = Threat::Danger;
            }
        }
    }
    return level;
}

// Threat is shown as a thick ring around the outside (not a full-screen tint)
// so the dark background stays clear for the chart overlay. Brighter, more
// saturated than the old fill colours so the ring reads at a glance.
lv_color_t bgFor(Threat w) {
    switch (w) {
        case Threat::Safe:   return lv_color_hex(0x12B021);  // green
        case Threat::Alert:  return lv_color_hex(0xC8C800);  // yellow (R=G, not amber)
        case Threat::Danger: return lv_color_hex(0xE00000);  // red
        default:             return lv_color_black();
    }
}

void ring(int r, lv_color_t c, int w) {
    lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
    d.color = c; d.width = w; d.opa = LV_OPA_COVER;
    lv_canvas_draw_arc(g_canvas, kCx, kCy, r, 0, 360, &d);
}
void line(int x1, int y1, int x2, int y2, lv_color_t c, int w) {
    lv_point_t p[2] = {{(lv_coord_t)x1, (lv_coord_t)y1},
                       {(lv_coord_t)x2, (lv_coord_t)y2}};
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = c; d.width = w; d.opa = LV_OPA_COVER;
    lv_canvas_draw_line(g_canvas, p, 2, &d);
}
void text(int x, int y, const char* s, lv_color_t c, const lv_font_t* f) {
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.color = c; d.font = f;
    lv_canvas_draw_text(g_canvas, x, y, 140, &d, s);
}
// No bold Montserrat is built in, so fake it by overstriking 1 px to the right.
void textB(int x, int y, const char* s, lv_color_t c, const lv_font_t* f) {
    text(x, y, s, c, f);
    text(x + 1, y, s, c, f);
}
void vessel(int cx, int cy, double hdg, double len, lv_color_t col) {
    const double t = hdg * kD2R, cs = std::cos(t), sn = std::sin(t);
    auto map = [&](double lx, double ly) {
        return lv_point_t{(lv_coord_t)std::lround(cx + lx * cs + ly * sn),
                          (lv_coord_t)std::lround(cy + lx * sn - ly * cs)};
    };
    lv_point_t tri[3] = {map(0, len), map(-len * 0.6, -len * 0.6),
                         map(len * 0.6, -len * 0.6)};
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = col; d.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_polygon(g_canvas, tri, 3, &d);
}
void dot(int x, int y, int r, lv_color_t col) {
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = col; d.bg_opa = LV_OPA_COVER; d.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(g_canvas, x - r, y - r, 2 * r, 2 * r, &d);
}
double niceStepNm(double t) {
    const double s[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 20, 50};
    for (double v : s) if (v >= t) return v;
    return 50;
}

// Chart overlay — nautical styling: filled land (sand) + depth-banded water
// (shades of blue, shallower = darker) over the pale "deep water" background,
// with coastline + shipping lanes as lines on top. Equirectangular projection
// (cheap, exact enough at radar ranges) + range-clip. Two passes so the line
// work lands on top of the area fills.
int g_chSeg = 0;            // chart features drawn last frame (status line)
lv_point_t g_poly[600];     // scratch for one projected feature

// DRVAL1 (area min depth, m) -> fill colour. false for water at/beyond the
// deep-water cutoff (left as the pale background).
bool depthBandColor(float drval, uint8_t cutoff, lv_color_t* out) {
    if (std::isnan(drval) || drval >= (float)cutoff) return false;
    if      (drval <= 0.0f)  *out = lv_color_hex(0x8FCBA0);  // drying (green)
    else if (drval <= 2.0f)  *out = lv_color_hex(0x4F97CE);  // 0–2 m  (darkest)
    else if (drval <= 5.0f)  *out = lv_color_hex(0x86BCE2);  // 2–5 m
    else if (drval <= 10.0f) *out = lv_color_hex(0xBCDDF2);  // 5–10 m
    else                     *out = lv_color_hex(0xDCEDF9);  // 10 m–cutoff
    return true;
}

// Project a feature's lat/lon points into g_poly (screen px); set anyIn if any
// point falls inside the clip window. Coords clamped to keep lv_coord_t sane.
uint16_t projectPoly(const chart::Feature& f, double ownLat, double ownLon,
                     double scale, double coslat, double clip, bool* anyIn) {
    const uint16_t n = f.npts > 600 ? 600 : f.npts;
    *anyIn = false;
    for (uint16_t i = 0; i < n; ++i) {
        const double dN = (f.pts[2 * i]     - ownLat) * 60.0;
        const double dE = (f.pts[2 * i + 1] - ownLon) * 60.0 * coslat;
        int x = kCx + (int)std::lround(dE * scale);
        int y = kCy - (int)std::lround(dN * scale);
        g_poly[i].x = (lv_coord_t)(x < -15000 ? -15000 : x > 15000 ? 15000 : x);
        g_poly[i].y = (lv_coord_t)(y < -15000 ? -15000 : y > 15000 ? 15000 : y);
        if (std::fabs(dN) < clip && std::fabs(dE) < clip) *anyIn = true;
    }
    return n;
}

// Even-odd scanline fill straight into the canvas buffer. Bounded to the
// screen and crash-proof, unlike lv_canvas_draw_polygon on raw CM93 rings
// (concave / self-intersecting / far off-canvas points).
void fillPoly(uint16_t n, lv_color_t col) {
    if (n < 3 || !g_buf) return;
    int ymin = kH, ymax = 0;
    for (uint16_t i = 0; i < n; ++i) {
        if (g_poly[i].y < ymin) ymin = g_poly[i].y;
        if (g_poly[i].y > ymax) ymax = g_poly[i].y;
    }
    if (ymin < 0) ymin = 0;
    if (ymax > kH - 1) ymax = kH - 1;
    for (int y = ymin; y <= ymax; ++y) {
        float xs[48]; int m = 0;
        for (uint16_t i = 0, j = n - 1; i < n; j = i++) {
            const int yi = g_poly[i].y, yj = g_poly[j].y;
            if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
                const float x = g_poly[i].x +
                    (float)(y - yi) / (float)(yj - yi) * (g_poly[j].x - g_poly[i].x);
                if (m < 48) xs[m++] = x;
            }
        }
        for (int a = 1; a < m; ++a) {     // insertion sort the crossings
            const float v = xs[a]; int b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; --b; }
            xs[b + 1] = v;
        }
        lv_color_t* row = g_buf + (size_t)y * kW;
        for (int k = 0; k + 1 < m; k += 2) {
            int x0 = (int)std::ceil(xs[k]), x1 = (int)std::floor(xs[k + 1]);
            if (x0 < 0) x0 = 0;
            if (x1 > kW - 1) x1 = kW - 1;
            for (int x = x0; x <= x1; ++x) row[x] = col;
        }
    }
}

void strokePoly(uint16_t n, lv_color_t col, int w) {
    for (uint16_t i = 1; i < n; ++i)
        line(g_poly[i - 1].x, g_poly[i - 1].y, g_poly[i].x, g_poly[i].y, col, w);
}

void drawChart(double ownLat, double ownLon, double scale, double rangeNm) {
    g_chSeg = 0;
    if (!chart::ensureCell(ownLat, ownLon)) return;
    const DeviceSettings& ds = devsettings::get();
    const double coslat = std::cos(ownLat * kD2R);
    const double clip   = rangeNm * 1.25;

    const lv_color_t sand    = lv_color_hex(0xE6D9A8);   // land
    const lv_color_t coastc  = lv_color_hex(0x35434F);   // coastline
    const lv_color_t tssGrey = lv_color_hex(0x8C949C);   // shipping lanes

    // Pass 1 — filled areas (depth bands + land) under the lines.
    chart::rewind();
    chart::Feature f;
    while (chart::next(f)) {
        if (!(ds.chartLayers & (1 << f.layer)) || !(f.flags & 0x01)) continue;
        lv_color_t col;
        if (f.layer == CHART_DEPTH) {
            if (!depthBandColor(f.depth, ds.depthThreshM, &col)) continue;
        } else if (f.layer == CHART_LAND) {
            col = sand;
        } else continue;
        bool anyIn;
        const uint16_t n = projectPoly(f, ownLat, ownLon, scale, coslat, clip, &anyIn);
        if (anyIn) { fillPoly(n, col); ++g_chSeg; }
    }
    // Pass 2 — coastline + shipping lanes on top.
    chart::rewind();
    while (chart::next(f)) {
        if (!(ds.chartLayers & (1 << f.layer))) continue;
        lv_color_t col;
        if (f.layer == CHART_COASTLINE) col = coastc;
        else if (f.layer == CHART_TSS)  col = tssGrey;
        else continue;
        bool anyIn;
        const uint16_t n = projectPoly(f, ownLat, ownLon, scale, coslat, clip, &anyIn);
        if (anyIn) { strokePoly(n, col, 2); ++g_chSeg; }
    }
}

}  // namespace

int assessWorst(AisTargetStore& store, double ownLat, double ownLon,
                double ownCogDeg, double ownSogKn) {
    AisTarget t[AisTargetStore::CAPACITY];
    const size_t n = store.snapshotByRecency(t, AisTargetStore::CAPACITY);
    Threat worst = Threat::None;
    for (size_t i = 0; i < n; ++i) {
        if (t[i].lat_deg == 0.0 && t[i].lon_deg == 0.0) continue;
        double rng, brg;
        rangeBearing(ownLat, ownLon, t[i].lat_deg, t[i].lon_deg, &rng, &brg);
        if (aisfilter::hidden(t[i].nav_status, rng, devsettings::get().rangeCapNm,
                              devsettings::get().hideAnchored)) continue;
        const Threat lv = assess(t[i], ownLat, ownLon, ownCogDeg, ownSogKn, rng);
        if ((uint8_t)lv > (uint8_t)worst) worst = lv;
    }
    return (int)worst;
}

void begin() {
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR(kW, kH);
    g_buf = (lv_color_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_buf) { Serial.println("[radar] PSRAM canvas alloc failed"); return; }
    g_canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(g_canvas, g_buf, kW, kH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(g_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_fill_bg(g_canvas, lv_color_black(), LV_OPA_COVER);
}

void draw(AisTargetStore& store, double ownLat, double ownLon,
          double ownCogDeg, double ownSogKn) {
    if (!g_canvas) return;

    // Light "chart" theme: dark, saturated marks that read on the pale chart.
    const lv_color_t ringc = lv_color_hex(0x6E7A86);   // range rings
    const lv_color_t blip  = lv_color_hex(0x14202B);   // safe target
    const lv_color_t dimc  = lv_color_hex(0x8A929A);   // stale target
    const lv_color_t ownc  = lv_color_hex(0xC00060);   // own ship (magenta)
    const lv_color_t txtc  = lv_color_hex(0x0E1C28);   // labels

    AisTarget t[AisTargetStore::CAPACITY];
    const size_t n = store.snapshotByRecency(t, AisTargetStore::CAPACITY);

    double rng[AisTargetStore::CAPACITY], brg[AisTargetStore::CAPACITY];
    double maxNm = 0.5;
    for (size_t i = 0; i < n; ++i) {
        if (t[i].lat_deg != 0.0 || t[i].lon_deg != 0.0) {
            rangeBearing(ownLat, ownLon, t[i].lat_deg, t[i].lon_deg, &rng[i], &brg[i]);
            if (aisfilter::hidden(t[i].nav_status, rng[i],
                                  devsettings::get().rangeCapNm,
                                  devsettings::get().hideAnchored)) { rng[i] = NAN; continue; }
            if (rng[i] > maxNm) maxNm = rng[i];
        } else { rng[i] = NAN; brg[i] = NAN; }
    }

    const Threat worst = (n == 0) ? Threat::None
        : (Threat)assessWorst(store, ownLat, ownLon, ownCogDeg, ownSogKn);
    lv_canvas_fill_bg(g_canvas, lv_color_hex(0xE9F1F7), LV_OPA_COVER);  // deep water

    // With the chart on, don't zoom tighter than ~2 NM, so nearby coast/depth
    // is visible even when no AIS targets are driving the range.
    const bool chartOn = devsettings::get().chartLayers != 0
                         && chart::ensureCell(ownLat, ownLon);
    if (chartOn && maxNm < 1.5) maxNm = 1.5;

    const double rangeNm = niceStepNm(maxNm * 1.15);
    const double scale = kR / rangeNm;

    drawChart(ownLat, ownLon, scale, rangeNm);   // chart under the AIS plot

    for (int k = 1; k <= 3; ++k) ring(kR * k / 3, ringc, 2);
    line(kCx, kCy - kR, kCx, kCy - kR + 16, ringc, 2);
    textB(kCx - 8, kCy - kR + 14, "N", txtc, &lv_font_montserrat_20);
    char rb[24]; snprintf(rb, sizeof(rb), "%.3g NM", rangeNm);
    textB(kCx - 34, kCy + kR - 28, rb, txtc, &lv_font_montserrat_20);

    const uint32_t now = millis();
    for (size_t i = 0; i < n; ++i) {
        if (std::isnan(rng[i]) || rng[i] > rangeNm) continue;
        const double br = brg[i] * kD2R;
        const int x = kCx + (int)std::lround(rng[i] * scale * std::sin(br));
        const int y = kCy - (int)std::lround(rng[i] * scale * std::cos(br));
        const bool stale = (now - t[i].last_seen_ms) > kDimAfterMs;
        // Colour each contact by its own threat so the modes are visible:
        // safe = dark, alert = yellow, danger = red (the outer ring shows worst).
        const Threat tlv = assess(t[i], ownLat, ownLon, ownCogDeg, ownSogKn, rng[i]);
        const lv_color_t c = stale ? dimc
                           : (tlv > Threat::Safe ? bgFor(tlv) : blip);

        if (t[i].sog_kn >= 0 && t[i].cog_deg >= 0 && t[i].sog_kn > 0.1) {
            const double d = t[i].sog_kn * kProjHr;
            const double cr = t[i].cog_deg * kD2R;
            line(x, y, x + (int)std::lround(d * scale * std::sin(cr)),
                 y - (int)std::lround(d * scale * std::cos(cr)), c, 2);
        }
        if (t[i].cog_deg >= 0) vessel(x, y, t[i].cog_deg, 9, c);
        else                   dot(x, y, 5, c);

        char lbl[12];
        if (t[i].name[0]) snprintf(lbl, sizeof(lbl), "%.8s", t[i].name);
        else snprintf(lbl, sizeof(lbl), "%u", (unsigned)(t[i].mmsi % 100000));
        textB(x + 8, y - 9, lbl, txtc, &lv_font_montserrat_16);
    }

    vessel(kCx, kCy, std::isnan(ownCogDeg) ? 0 : ownCogDeg, 12, ownc);

    // Threat as a thick ring around the outer edge (replaces the bg tint).
    if (worst != Threat::None) ring(kH / 2 - 10, bgFor(worst), 16);

    char hud[16]; snprintf(hud, sizeof(hud), "%u tgt", (unsigned)n);
    text(10, 10, hud, txtc, &lv_font_montserrat_16);
    char pos[40]; snprintf(pos, sizeof(pos), "%.5f\n%.5f", ownLat, ownLon);
    text(10, 32, pos, ownc, &lv_font_montserrat_12);

    // Chart status (diagnostic) — kept on the vertical axis so it stays inside
    // the round display (the corners are off-glass).
    char cs[44];
    if (chart::featureCount() > 0)
        snprintf(cs, sizeof(cs), "chart %uf %dd",
                 (unsigned)chart::featureCount(), g_chSeg);
    else
        snprintf(cs, sizeof(cs), "NO TILE %08u", (unsigned)chart::cellId());
    textB(kCx - 52, kCy + 52, cs, txtc, &lv_font_montserrat_16);

    lv_obj_invalidate(g_canvas);
}

}  // namespace radar
