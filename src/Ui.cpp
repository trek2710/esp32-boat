// LVGL instrument UI — three screens, swipe to cycle:
//   0. Overview — classic-boating wind compass + big SOG + big wind speed
//   1. Data     — every value from BoatState in a compact grid
//   2. Debug    — rolling NMEA 2000 PGN log (populated in sim + real builds)
//
// DISPLAY DRIVER:
// The Waveshare ESP32-S3-Touch-LCD-2.1 uses a QSPI-interfaced ST77916 panel
// (480×480, round) plus a CH422G I2C I/O expander that gates LCD reset,
// touch reset, and backlight enable. We drive both from src/display/ —
// St77916Panel handles the QSPI bus + vendor init + pixel blasts, Ch422g
// handles the expander-gated resets. Nothing in the LVGL widget code below
// knows or cares about this — the only integration point is flush_cb.
// Capacitive touch (CST820) is not wired in v1; the UI is screen-only
// with programmatic page cycling. A CST820 driver + LVGL input device will
// land in a follow-up.

#include "Ui.h"

#include "config.h"

#if DISPLAY_SAFE_MODE
// -----------------------------------------------------------------------------
// SAFE MODE — stub implementation. No LVGL, no LovyanGFX. Lets us prove the
// firmware boots on new hardware without fighting any display-driver or LVGL
// header wiring issues. Pair with DISPLAY_SAFE_MODE=1 in main.cpp so the
// caller never invokes these stubs' side-effects.
// -----------------------------------------------------------------------------
namespace ui {
void begin(BoatState& /*state*/) {
    // Intentional no-op. main.cpp does not call us when DISPLAY_SAFE_MODE=1.
}
uint32_t tick() {
    return 100;  // idle hint; main.cpp also does not call us in safe mode.
}
}  // namespace ui

#else  // !DISPLAY_SAFE_MODE — the real LVGL UI.

#include <Arduino.h>
#include <Wire.h>
// NOTE: include real LVGL via the lvgl/ subpath. Historical reason kept in
// case we ever re-add LovyanGFX for anything: that library ships its own
// lvgl.h shim that hijacked the unqualified include. Going via the lvgl/
// subdir remains unambiguous.
#include <lvgl/lvgl.h>

#include <cstdio>

#include "display/ch422g.h"
#include "display/display_pins.h"
#include "display/st77916_panel.h"

namespace ui {
namespace {

// --- Display driver -------------------------------------------------------

// Real driver now. The CH422G expander must be initialized before the panel
// (it gates the panel's RESET line); we keep both as file-local statics.
display::Ch422g       g_expander;
display::St77916Panel g_panel;

lv_disp_draw_buf_t draw_buf;
lv_color_t*        buf1 = nullptr;
lv_color_t*        buf2 = nullptr;

BoatState* g_state = nullptr;

// --- Page state -------------------------------------------------------------

constexpr int kNumPages = 3;

// ---- Overview page ----
struct OverviewPage {
    lv_obj_t* root;
    lv_obj_t* wind_meter;               // lv_meter (compass rose)
    lv_meter_indicator_t* wind_needle;  // AWA needle
    lv_obj_t* wind_center_lbl;          // AWS inside the compass
    lv_obj_t* sog_big_lbl;              // big SOG number
    lv_obj_t* sog_unit_lbl;             // "kn SOG" below the big number
    lv_obj_t* hdg_small_lbl;            // HDG/COG under SOG
};
OverviewPage overview;

// ---- Data page ----
struct DataPage {
    lv_obj_t* root;
    lv_obj_t* body_lbl;                 // one multi-line label with everything
};
DataPage data_pg;

// ---- Debug page ----
struct DebugPage {
    lv_obj_t* root;
    lv_obj_t* header_lbl;               // "PGN LOG (N)"
    lv_obj_t* body_lbl;                 // recent entries, newest first
};
DebugPage debug_pg;

lv_obj_t* pages[kNumPages] = { nullptr, nullptr, nullptr };
int       current_page = 0;

// --- Helpers ----------------------------------------------------------------

void styleScreen(lv_obj_t* scr) {
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* makeLabel(lv_obj_t* parent,
                    const lv_font_t* font,
                    lv_color_t color,
                    const char* initial_text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, initial_text);
    lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    return lbl;
}

// --- Page construction ------------------------------------------------------

void buildOverviewPage() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleScreen(scr);
    overview.root = scr;

    // Wind meter (compass rose) — 280×280, positioned at the top of the
    // round 480×480 display. 0° at the top = bow; positive = starboard.
    lv_obj_t* meter = lv_meter_create(scr);
    lv_obj_set_size(meter, 280, 280);
    lv_obj_align(meter, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(meter, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(meter, 0, LV_PART_MAIN);

    lv_meter_scale_t* scale = lv_meter_add_scale(meter);
    // 360° scale, 0 at top, clockwise. Rotate 270 so angle 0 points up.
    lv_meter_set_scale_ticks(meter, scale, 37, 2, 8, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter, scale, 3, 4, 14, lv_color_white(), 15);
    lv_meter_set_scale_range(meter, scale, 0, 360, 360, 270);
    overview.wind_needle = lv_meter_add_needle_line(
        meter, scale, 4, lv_palette_main(LV_PALETTE_RED), -10);
    lv_meter_set_indicator_value(meter, overview.wind_needle, 0);
    overview.wind_meter = meter;

    // Wind speed (AWS) — shown large inside the compass centre.
    overview.wind_center_lbl = makeLabel(meter, &lv_font_montserrat_48,
                                         lv_color_white(), "--");
    lv_obj_align(overview.wind_center_lbl, LV_ALIGN_CENTER, 0, -6);

    // "kn AWS" unit just below the wind speed.
    lv_obj_t* aws_unit = makeLabel(meter, &lv_font_montserrat_20,
                                   lv_palette_main(LV_PALETTE_GREY), "kn AWS");
    lv_obj_align(aws_unit, LV_ALIGN_CENTER, 0, 38);

    // SOG — very large, lower part of the screen.
    overview.sog_big_lbl = makeLabel(scr, &lv_font_montserrat_48,
                                     lv_color_white(), "--");
    lv_obj_align(overview.sog_big_lbl, LV_ALIGN_BOTTOM_MID, 0, -80);

    overview.sog_unit_lbl = makeLabel(scr, &lv_font_montserrat_20,
                                      lv_palette_main(LV_PALETTE_GREY), "kn SOG");
    lv_obj_align(overview.sog_unit_lbl, LV_ALIGN_BOTTOM_MID, 0, -50);

    // Heading / COG — smaller, at the very bottom.
    overview.hdg_small_lbl = makeLabel(scr, &lv_font_montserrat_20,
                                       lv_palette_main(LV_PALETTE_GREY),
                                       "HDG --   COG --");
    lv_obj_align(overview.hdg_small_lbl, LV_ALIGN_BOTTOM_MID, 0, -15);
}

void buildDataPage() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleScreen(scr);
    data_pg.root = scr;

    lv_obj_t* title = makeLabel(scr, &lv_font_montserrat_28,
                                lv_color_white(), "DATA");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    data_pg.body_lbl = makeLabel(scr, &lv_font_montserrat_20,
                                 lv_color_white(), "loading...");
    lv_label_set_long_mode(data_pg.body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(data_pg.body_lbl, 440);
    lv_obj_set_style_text_line_space(data_pg.body_lbl, 4, LV_PART_MAIN);
    lv_obj_align(data_pg.body_lbl, LV_ALIGN_TOP_MID, 0, 70);
}

void buildDebugPage() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleScreen(scr);
    debug_pg.root = scr;

    debug_pg.header_lbl = makeLabel(scr, &lv_font_montserrat_20,
                                    lv_color_white(), "PGN LOG (0)");
    lv_obj_align(debug_pg.header_lbl, LV_ALIGN_TOP_MID, 0, 20);

    debug_pg.body_lbl = makeLabel(scr, &lv_font_montserrat_14,
                                  lv_color_white(), "(waiting for traffic)");
    lv_label_set_long_mode(debug_pg.body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(debug_pg.body_lbl, 460);
    lv_obj_set_style_text_line_space(debug_pg.body_lbl, 2, LV_PART_MAIN);
    lv_obj_align(debug_pg.body_lbl, LV_ALIGN_TOP_LEFT, 10, 60);
}

// --- Swipe handler (LVGL screen gestures) ----------------------------------

void swipeHandler(lv_event_t* /*e*/) {
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT)  current_page = (current_page + 1) % kNumPages;
    if (dir == LV_DIR_RIGHT) current_page = (current_page + kNumPages - 1) % kNumPages;
    lv_scr_load(pages[current_page]);
    // Re-attach gesture event to the newly-active screen so swipes keep working.
    lv_obj_add_event_cb(lv_scr_act(), swipeHandler, LV_EVENT_GESTURE, nullptr);
}

// --- Refresh ---------------------------------------------------------------

void refreshOverview(const Instruments& s) {
    // Wind needle: AWA is -180..180 with + starboard. Meter scale is 0..360
    // with 0=bow, so shift by +360 when negative.
    if (!isnan(s.awa)) {
        double deg = s.awa < 0 ? s.awa + 360.0 : s.awa;
        lv_meter_set_indicator_value(overview.wind_meter,
                                     overview.wind_needle,
                                     static_cast<int32_t>(deg));
    }

    // AWS in the meter centre.
    if (isnan(s.aws)) {
        lv_label_set_text(overview.wind_center_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", s.aws);
        lv_label_set_text(overview.wind_center_lbl, buf);
    }

    // Big SOG
    if (isnan(s.sog)) {
        lv_label_set_text(overview.sog_big_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", s.sog);
        lv_label_set_text(overview.sog_big_lbl, buf);
    }

    // HDG + COG secondary line
    char hdg_buf[16], cog_buf[16];
    if (isnan(s.heading_true_deg)) snprintf(hdg_buf, sizeof(hdg_buf), "--");
    else snprintf(hdg_buf, sizeof(hdg_buf), "%.0f\xC2\xB0", s.heading_true_deg);
    if (isnan(s.cog))              snprintf(cog_buf, sizeof(cog_buf), "--");
    else snprintf(cog_buf, sizeof(cog_buf), "%.0f\xC2\xB0", s.cog);
    lv_label_set_text_fmt(overview.hdg_small_lbl,
                          "HDG %s   COG %s", hdg_buf, cog_buf);
}

// Format "value or --"; caller provides buffer, precision format string.
void fmtOrDash(char* out, size_t n, double v, const char* fmt) {
    if (isnan(v)) snprintf(out, n, "--");
    else          snprintf(out, n, fmt, v);
}

void refreshData(const Instruments& s, size_t ais_count) {
    char lat[12], lon[12], sog[12], cog[12];
    char awa[12], aws[12], twa[12], tws[12];
    char dep[12], tmp[12], hdg[12], stw[12];

    fmtOrDash(lat, sizeof(lat), s.lat,              "%.4f");
    fmtOrDash(lon, sizeof(lon), s.lon,              "%.4f");
    fmtOrDash(sog, sizeof(sog), s.sog,              "%.1f");
    fmtOrDash(cog, sizeof(cog), s.cog,              "%.0f");
    fmtOrDash(awa, sizeof(awa), s.awa,              "%+.0f");
    fmtOrDash(aws, sizeof(aws), s.aws,              "%.1f");
    fmtOrDash(twa, sizeof(twa), s.twa,              "%+.0f");
    fmtOrDash(tws, sizeof(tws), s.tws,              "%.1f");
    fmtOrDash(dep, sizeof(dep), s.depth_m,          "%.1f");
    fmtOrDash(tmp, sizeof(tmp), s.water_temp_c,     "%.1f");
    fmtOrDash(hdg, sizeof(hdg), s.heading_true_deg, "%.0f");
    fmtOrDash(stw, sizeof(stw), s.stw,              "%.1f");

    char body[512];
    snprintf(body, sizeof(body),
             "LAT %s   LON %s\n"
             "SOG %s kn   COG %s\xC2\xB0\n"
             "AWA %s\xC2\xB0   AWS %s kn\n"
             "TWA %s\xC2\xB0   TWS %s kn\n"
             "DEPTH %s m   TEMP %s\xC2\xB0""C\n"
             "HDG %s\xC2\xB0   STW %s kn\n"
             "AIS targets: %u",
             lat, lon,
             sog, cog,
             awa, aws,
             twa, tws,
             dep, tmp,
             hdg, stw,
             static_cast<unsigned>(ais_count));
    lv_label_set_text(data_pg.body_lbl, body);
}

void refreshDebug() {
    if (!g_state) return;
    const uint32_t total = g_state->pgnLogTotal();
    lv_label_set_text_fmt(debug_pg.header_lbl, "PGN LOG (%lu)",
                          static_cast<unsigned long>(total));

    auto log = g_state->pgnLogSnapshot(); // newest-first

    // Keep it compact: show up to 18 newest non-empty entries. At montserrat_14
    // with ~460px width that's about as much as fits on the round display.
    constexpr size_t kMaxShown = 18;
    char body[1400];
    size_t off = 0;
    body[0] = '\0';

    const uint32_t now = millis();
    size_t shown = 0;
    for (const auto& e : log) {
        if (shown >= kMaxShown) break;
        if (e.pgn == 0) continue;
        const uint32_t age = now - e.t_ms;
        int n = snprintf(body + off, sizeof(body) - off,
                         "%6lu  %4lu ms  %s\n",
                         static_cast<unsigned long>(e.pgn),
                         static_cast<unsigned long>(age),
                         e.summary);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(body) - off) break;
        off += n;
        shown++;
    }
    if (shown == 0) {
        snprintf(body, sizeof(body), "(waiting for traffic)");
    }
    lv_label_set_text(debug_pg.body_lbl, body);
}

void refreshFromState() {
    if (!g_state) return;
    const Instruments s   = g_state->snapshot();
    const auto        ais = g_state->aisSnapshot();
    size_t ais_count = 0;
    for (const auto& t : ais) if (t.mmsi != 0) ais_count++;

    // Only refresh the currently visible page — cheaper, and hidden pages will
    // refresh next time they're shown.
    switch (current_page) {
        case 0: refreshOverview(s);        break;
        case 1: refreshData(s, ais_count); break;
        case 2: refreshDebug();            break;
    }
}

// --- Display driver glue ---------------------------------------------------

// LVGL -> ST77916 glue. LVGL hands us a partial-screen rectangle of RGB565
// pixels (LV_COLOR_DEPTH=16, LV_COLOR_16_SWAP=1); we forward it straight to
// the panel. lv_disp_flush_ready tells LVGL the buffer is reusable.
void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    g_panel.drawBitmap(area->x1, area->y1, area->x2, area->y2, color_p);
    lv_disp_flush_ready(drv);
}

}  // namespace

// ---------------------------------------------------------------------------

void begin(BoatState& state) {
    g_state = &state;

    // Each step logs before+after so a crash pinpoints the exact stage. Flush
    // is critical — USB-CDC drops its last buffered bytes on a hard reset, so
    // without flush we'd never know where we died.
    auto step = [](const char* tag) {
        Serial.print(F("[ui] "));
        Serial.println(tag);
        Serial.flush();
    };

    step("Wire.begin()");
    Wire.begin(display::I2C_PIN_SDA, display::I2C_PIN_SCL, display::I2C_FREQ_HZ);

    step("CH422G.begin()");
    if (!g_expander.begin()) {
        Serial.println(F("[ui] CH422G not responding on I2C — backlight + resets "
                          "won't be driven. Check I2C pins / board rev."));
        Serial.flush();
    }

    step("ST77916.begin()");
    if (!g_panel.begin(g_expander)) {
        Serial.println(F("[ui] ST77916 bring-up failed — continuing without "
                          "pixels; LVGL will still tick."));
        Serial.flush();
    } else {
        step("ST77916 fillColor(black)");
        g_panel.fillColor(0x0000);
    }

    step("lv_init()");
    lv_init();

    step("ps_malloc framebuffers");
    const size_t line_buf_px = DISPLAY_WIDTH * 40;
    buf1 = static_cast<lv_color_t*>(ps_malloc(line_buf_px * sizeof(lv_color_t)));
    buf2 = static_cast<lv_color_t*>(ps_malloc(line_buf_px * sizeof(lv_color_t)));
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, line_buf_px);

    step("lv_disp_drv_register");
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = DISPLAY_WIDTH;
    disp_drv.ver_res  = DISPLAY_HEIGHT;
    disp_drv.flush_cb = flushCb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    step("build pages");
    buildOverviewPage();
    buildDataPage();
    buildDebugPage();
    pages[0] = overview.root;
    pages[1] = data_pg.root;
    pages[2] = debug_pg.root;

    lv_scr_load(pages[0]);
    lv_obj_add_event_cb(lv_scr_act(), swipeHandler, LV_EVENT_GESTURE, nullptr);
    step("ready");
}

uint32_t tick() {
    refreshFromState();
    return lv_timer_handler();
}

}  // namespace ui

#endif  // DISPLAY_SAFE_MODE
