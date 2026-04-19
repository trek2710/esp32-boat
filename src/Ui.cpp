// Minimal LVGL instrument UI.
//
// This file deliberately stays small: it sets up LVGL with the LovyanGFX
// driver, builds five pages with plain labels, and updates them from
// BoatState once per tick. Visual polish (gauges, dials, round-layout tweaks)
// comes in v1.x once we have the real hardware to iterate against.

#include "Ui.h"

#include "config.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

namespace ui {
namespace {

// ---------------------------------------------------------------------------
// LovyanGFX configuration for Waveshare ESP32-S3-Touch-LCD-2.1.
// These pin values come from the Waveshare wiki example; verify against your
// exact board revision before trusting them for flashing.
// ---------------------------------------------------------------------------
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7701       panel_; // Waveshare 2.1" round uses ST7701 or ST77916.
                                     // Swap for Panel_ST77916 if the display stays blank.
    lgfx::Bus_Parallel16     bus_;   // Placeholder; real board is QSPI — replace with
                                     // Bus_SPI / custom QSPI config per Waveshare demo.
    lgfx::Light_PWM          light_;
    lgfx::Touch_CST820       touch_;

public:
    LGFX() {
        // Stub config — the scaffolded build just needs these to link.
        // At hardware-bringup time, replace with the exact values from
        // Waveshare's factory demo (QSPI pins, initialisation sequence).
        auto pcfg = panel_.config();
        pcfg.memory_width  = DISPLAY_WIDTH;
        pcfg.memory_height = DISPLAY_HEIGHT;
        pcfg.panel_width   = DISPLAY_WIDTH;
        pcfg.panel_height  = DISPLAY_HEIGHT;
        pcfg.offset_rotation = 0;
        panel_.config(pcfg);

        setPanel(&panel_);
    }
};

LGFX                  gfx;
lv_disp_draw_buf_t    draw_buf;
// Dynamically allocated from PSRAM in begin() to keep DRAM free.
lv_color_t*           buf1 = nullptr;
lv_color_t*           buf2 = nullptr;

BoatState*            g_state = nullptr;

// Screens + labels we'll update each tick.
struct Page {
    lv_obj_t* root;
    lv_obj_t* labels[6];   // per-page — unused slots left nullptr
};
Page pages[5] = {};
int  current_page = 0;

lv_obj_t* buildPage(const char* title) {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

    lv_obj_t* title_lbl = lv_label_create(scr);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 40);
    return scr;
}

lv_obj_t* addStat(lv_obj_t* parent, const char* label, int y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text_fmt(lbl, "%s: --", label);
    lv_obj_set_style_text_color(lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
    return lbl;
}

void buildAllPages() {
    // GPS
    pages[0].root      = buildPage("GPS");
    pages[0].labels[0] = addStat(pages[0].root, "LAT",  110);
    pages[0].labels[1] = addStat(pages[0].root, "LON",  160);
    pages[0].labels[2] = addStat(pages[0].root, "SOG",  250);
    pages[0].labels[3] = addStat(pages[0].root, "COG",  300);

    // Wind
    pages[1].root      = buildPage("WIND");
    pages[1].labels[0] = addStat(pages[1].root, "AWA",  120);
    pages[1].labels[1] = addStat(pages[1].root, "AWS",  170);
    pages[1].labels[2] = addStat(pages[1].root, "TWA",  260);
    pages[1].labels[3] = addStat(pages[1].root, "TWS",  310);

    // Depth / temp
    pages[2].root      = buildPage("DEPTH");
    pages[2].labels[0] = addStat(pages[2].root, "DEPTH", 150);
    pages[2].labels[1] = addStat(pages[2].root, "TEMP",  250);

    // Heading / STW
    pages[3].root      = buildPage("HEADING");
    pages[3].labels[0] = addStat(pages[3].root, "HDG", 150);
    pages[3].labels[1] = addStat(pages[3].root, "STW", 250);

    // AIS
    pages[4].root      = buildPage("AIS");
    pages[4].labels[0] = addStat(pages[4].root, "Targets", 150);
    // A proper list view comes in v1.x — for scaffold we just show count + closest MMSI.
}

// LVGL → LovyanGFX glue.
void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    const uint32_t w = area->x2 - area->x1 + 1;
    const uint32_t h = area->y2 - area->y1 + 1;
    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    gfx.writePixels(reinterpret_cast<uint16_t*>(color_p), w * h);
    gfx.endWrite();
    lv_disp_flush_ready(drv);
}

void readTouchCb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    uint16_t x = 0, y = 0;
    if (gfx.getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void swipeHandler(lv_event_t* e) {
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT)  current_page = (current_page + 1) % 5;
    if (dir == LV_DIR_RIGHT) current_page = (current_page + 4) % 5; // -1 mod 5
    lv_scr_load(pages[current_page].root);
}

// Convenience: format a double (NaN-safe).
void setOrDash(lv_obj_t* lbl, const char* key, double v, const char* fmt, const char* unit) {
    if (isnan(v)) {
        lv_label_set_text_fmt(lbl, "%s: --", key);
    } else {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), fmt, v);
        lv_label_set_text_fmt(lbl, "%s: %s %s", key, tmp, unit);
    }
}

void refreshFromState() {
    if (!g_state) return;
    auto s = g_state->snapshot();
    auto ais = g_state->aisSnapshot();

    setOrDash(pages[0].labels[0], "LAT", s.lat, "%.4f",  "\xC2\xB0");
    setOrDash(pages[0].labels[1], "LON", s.lon, "%.4f",  "\xC2\xB0");
    setOrDash(pages[0].labels[2], "SOG", s.sog, "%.1f",  "kn");
    setOrDash(pages[0].labels[3], "COG", s.cog, "%.0f",  "\xC2\xB0T");

    setOrDash(pages[1].labels[0], "AWA", s.awa, "%+.0f", "\xC2\xB0");
    setOrDash(pages[1].labels[1], "AWS", s.aws, "%.1f",  "kn");
    setOrDash(pages[1].labels[2], "TWA", s.twa, "%+.0f", "\xC2\xB0");
    setOrDash(pages[1].labels[3], "TWS", s.tws, "%.1f",  "kn");

    setOrDash(pages[2].labels[0], "DEPTH", s.depth_m,      "%.1f", "m");
    setOrDash(pages[2].labels[1], "TEMP",  s.water_temp_c, "%.1f", "\xC2\xB0""C");

    setOrDash(pages[3].labels[0], "HDG", s.heading_true_deg, "%.0f", "\xC2\xB0T");
    setOrDash(pages[3].labels[1], "STW", s.stw,              "%.1f", "kn");

    size_t ais_count = 0;
    for (auto& t : ais) if (t.mmsi != 0) ais_count++;
    lv_label_set_text_fmt(pages[4].labels[0], "Targets: %u", static_cast<unsigned>(ais_count));
}

}  // namespace

void begin(BoatState& state) {
    g_state = &state;

    gfx.begin();
    gfx.setRotation(0);
    gfx.fillScreen(TFT_BLACK);

    lv_init();

    // Two line-buffers in PSRAM (40 rows of 480 pixels each = ~38 KB).
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

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = readTouchCb;
    lv_indev_drv_register(&indev_drv);

    buildAllPages();
    lv_scr_load(pages[0].root);

    // Global swipe listener on the active screen.
    lv_obj_add_event_cb(lv_scr_act(), swipeHandler, LV_EVENT_GESTURE, nullptr);
}

uint32_t tick() {
    refreshFromState();
    return lv_timer_handler();
}

}  // namespace ui
