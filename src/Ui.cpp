// LVGL instrument UI — three screens, swipe to cycle:
//   0. Overview — classic-boating wind compass + big SOG + big wind speed
//   1. Data     — every value from BoatState in a compact grid
//   2. Debug    — rolling NMEA 2000 PGN log (populated in sim + real builds)
//
// NOTE ON THE DISPLAY DRIVER:
// The Waveshare ESP32-S3-Touch-LCD-2.1 uses a QSPI-interfaced ST77916 panel
// and a CST820 I2C touch controller — neither of which are supported by
// LovyanGFX 1.2.0 out of the box. For the v1 scaffold we use a *stub* LGFX
// class (Panel_GC9A01 on SPI with placeholder pins) so the firmware
// compiles and boots cleanly; the display itself won't light up on the real
// board until this class is replaced with either:
//   (a) a hand-written LovyanGFX Panel_ST77916 / Touch_CST820 subclass, or
//   (b) the Espressif ESP32_Display_Panel library (which has a ready-made
//       board preset for ESP32-S3-Touch-LCD-2.1).
// The simulated firmware is still useful without the display — the serial
// monitor will show boot / simulated data / timing, proving the sw stack
// runs on your hardware.

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
#include <LovyanGFX.hpp>
// See main.cpp / platformio.ini for why this is <lvgl/lvgl.h> and not <lvgl.h>:
// LovyanGFX ships its own lvgl.h shim that hijacks the unqualified include.
#include <lvgl/lvgl.h>

#include <cstdio>

namespace ui {
namespace {

// --- Display driver stub ----------------------------------------------------

// Stub LGFX — compiles cleanly on LovyanGFX 1.2.0 but does NOT drive the
// Waveshare 2.1" round. Replace before expecting pixels on the real panel.
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 panel_;
    lgfx::Bus_SPI      bus_;

public:
    LGFX() {
        { // Bus — placeholder SPI config.
            auto cfg = bus_.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = -1;
            cfg.pin_mosi    = -1;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = -1;
            bus_.config(cfg);
            panel_.setBus(&bus_);
        }
        { // Panel — declare 480×480 so LVGL lays out correctly; GC9A01's
          // real resolution is 240×240 but this class never actually drives
          // the panel in v1.
            auto cfg = panel_.config();
            cfg.pin_cs         = -1;
            cfg.pin_rst        = -1;
            cfg.pin_busy       = -1;
            cfg.memory_width   = DISPLAY_WIDTH;
            cfg.memory_height  = DISPLAY_HEIGHT;
            cfg.panel_width    = DISPLAY_WIDTH;
            cfg.panel_height   = DISPLAY_HEIGHT;
            cfg.offset_x       = 0;
            cfg.offset_y       = 0;
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable        = false;
            cfg.invert          = false;
            cfg.rgb_order       = false;
            cfg.dlen_16bit      = false;
            cfg.bus_shared      = false;
            panel_.config(cfg);
        }
        setPanel(&panel_);
    }
};

LGFX               gfx;
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

// LVGL -> LovyanGFX glue. The stub LGFX doesn't light up the real panel, but
// LVGL still calls flush_cb which we terminate so LVGL doesn't stall.
void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;
    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    gfx.writePixels(reinterpret_cast<uint16_t*>(color_p), w * h);
    gfx.endWrite();
    lv_disp_flush_ready(drv);
}

}  // namespace

// ---------------------------------------------------------------------------

void begin(BoatState& state) {
    g_state = &state;

    gfx.begin();
    gfx.setRotation(0);
    gfx.fillScreen(TFT_BLACK);

    lv_init();

    const size_t line_buf_px = DISPLAY_WIDTH * 40;
    buf1 = static_cast<lv_color_t*>(ps_malloc(line_buf_px * sizeof(lv_color_t)));
    buf2 = static_cast<lv_color_t*>(ps_malloc(line_buf_px * sizeof(lv_color_t)));
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, line_buf_px);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = DISPLAY_WIDTH;
    disp_drv.ver_res  = DISPLAY_HEIGHT;
    disp_drv.flush_cb = flushCb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // No touch driver in the v1 scaffold — Touch_CST820 isn't in LovyanGFX
    // 1.2.0. Once we have the real display driver up, we'll either write a
    // CST820 I2C driver or switch to a framework that includes one.

    buildOverviewPage();
    buildDataPage();
    buildDebugPage();
    pages[0] = overview.root;
    pages[1] = data_pg.root;
    pages[2] = debug_pg.root;

    lv_scr_load(pages[0]);
    lv_obj_add_event_cb(lv_scr_act(), swipeHandler, LV_EVENT_GESTURE, nullptr);
}

uint32_t tick() {
    refreshFromState();
    return lv_timer_handler();
}

}  // namespace ui

#endif  // DISPLAY_SAFE_MODE
