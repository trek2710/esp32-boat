// Minimal LVGL instrument UI.
//
// Five pages (GPS / Wind / Depth / Heading / AIS) with swipe-to-page. Values
// come from BoatState every tick.
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

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

namespace ui {
namespace {

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

struct Page {
    lv_obj_t* root;
    lv_obj_t* labels[6];
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
    pages[0].root      = buildPage("GPS");
    pages[0].labels[0] = addStat(pages[0].root, "LAT",  110);
    pages[0].labels[1] = addStat(pages[0].root, "LON",  160);
    pages[0].labels[2] = addStat(pages[0].root, "SOG",  250);
    pages[0].labels[3] = addStat(pages[0].root, "COG",  300);

    pages[1].root      = buildPage("WIND");
    pages[1].labels[0] = addStat(pages[1].root, "AWA",  120);
    pages[1].labels[1] = addStat(pages[1].root, "AWS",  170);
    pages[1].labels[2] = addStat(pages[1].root, "TWA",  260);
    pages[1].labels[3] = addStat(pages[1].root, "TWS",  310);

    pages[2].root      = buildPage("DEPTH");
    pages[2].labels[0] = addStat(pages[2].root, "DEPTH", 150);
    pages[2].labels[1] = addStat(pages[2].root, "TEMP",  250);

    pages[3].root      = buildPage("HEADING");
    pages[3].labels[0] = addStat(pages[3].root, "HDG", 150);
    pages[3].labels[1] = addStat(pages[3].root, "STW", 250);

    pages[4].root      = buildPage("AIS");
    pages[4].labels[0] = addStat(pages[4].root, "Targets", 150);
}

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

void swipeHandler(lv_event_t* /*e*/) {
    const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT)  current_page = (current_page + 1) % 5;
    if (dir == LV_DIR_RIGHT) current_page = (current_page + 4) % 5; // -1 mod 5
    lv_scr_load(pages[current_page].root);
}

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
    auto s   = g_state->snapshot();
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
    for (const auto& t : ais) if (t.mmsi != 0) ais_count++;
    lv_label_set_text_fmt(pages[4].labels[0], "Targets: %u",
                          static_cast<unsigned>(ais_count));
}

}  // namespace

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

    buildAllPages();
    lv_scr_load(pages[0].root);
    lv_obj_add_event_cb(lv_scr_act(), swipeHandler, LV_EVENT_GESTURE, nullptr);
}

uint32_t tick() {
    refreshFromState();
    return lv_timer_handler();
}

}  // namespace ui
