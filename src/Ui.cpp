// LVGL instrument UI — three screens, swipe to cycle:
//   0. Overview — classic-boating wind compass + big SOG + big wind speed
//   1. Data     — every value from BoatState in a compact grid
//   2. Debug    — rolling NMEA 2000 PGN log (populated in sim + real builds)
//
// DISPLAY DRIVER:
// The Waveshare ESP32-S3-Touch-LCD-2.1 on our bench is the ST7701 RGB + TCA9554
// variant (480×480, round). Rounds 1..9 incorrectly assumed it was the
// ST77916 QSPI + CH422G variant; round 9's I2C scan proved otherwise. The
// TCA9554 expander at 0x20 gates LCD reset, touch reset, backlight, and the
// panel's 3-wire-SPI CS line. src/display/Tca9554 handles the expander;
// src/display/St7701Panel (round 14) drives the panel: bit-banged 3-wire
// SPI init over GPIO1/2 with CS via TCA9554 IO3, then esp_lcd_panel_rgb
// for the 16 parallel data lines + HSYNC/VSYNC/DE/PCLK with a 480×480×2
// framebuffer in PSRAM. Nothing in the LVGL widget code below knows or
// cares — the only integration point is flushCb.
//
// Confirmation that a rewrite (not a wiring hunt) was the right move: the
// device shipped with a factory demo image that displayed a working settings
// UI when the user first powered it up over USB-C. Panel, backlight, touch
// — the hardware is fully functional. Our round-12 dark screen was purely a
// driver-protocol mismatch: ST77916 QSPI sequences sent to an ST7701 RGB
// panel render as zero pixels, and without a valid RGB pixel stream the
// LC layer is opaque, which is also why the TCA9554 bit-walk never lit
// the backlight regardless of which bit we toggled.
//
// Capacitive touch (CST820 at 0x15) is wired on the same I2C bus but not
// driven in v1; the UI is screen-only with programmatic page cycling. A
// CST820 driver + LVGL input device will land in a follow-up.

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

#include "display/cst820.h"
#include "display/display_pins.h"
#include "display/st7701_panel.h"
#include "display/tca9554.h"

namespace ui {
namespace {

// --- Display driver -------------------------------------------------------

// Real driver now. The TCA9554 expander must be initialized before the panel
// (it gates the panel's RESET, CS, and backlight lines); we keep both as
// file-local statics so LVGL's flush_cb can reach g_panel without plumbing.
display::Tca9554     g_expander;
display::St7701Panel g_panel;
display::Cst820      g_touch;

lv_disp_draw_buf_t draw_buf;
lv_color_t*        buf1 = nullptr;
lv_color_t*        buf2 = nullptr;

BoatState* g_state = nullptr;

// --- Page state -------------------------------------------------------------

constexpr int kNumPages = 3;

// ---- Overview page (round 40, B&G-reference compass-only) ----
//
// Round 40 dropped the perimeter tiles introduced in round 36. The user
// pointed at the middle section of the B&G/Raymarine reference image and
// asked for "exactly that" — i.e. the central instrument only:
//
//   * Outer dial: light-grey ring, ~60 px wide, with degree tick marks
//     and labels every 30° (000, 030, ..., 330). Reads clockwise so the
//     bow always points up at "000".
//   * Red/green close-hauled sectors at the top of the dial (the no-go
//     zone is fixed at ±20° around the bow for v1 — eventually it'll
//     follow TWA).
//   * Inner black disc covering the centre.
//   * Boat-outline silhouette in white, drawn with lv_line.
//   * White-bordered speed circle inside the hull, holding the big
//     BOAT-SPD value.
//   * "000" heading box at top of the inner area (current heading).
//   * Red AWA needle (apparent wind angle) + green reference needle.
//   * "°M" magnetic-indicator label at top-right of the inner area.
//   * "Var: 16°W" magnetic-variation label at bottom-left.
//
// The 10 metrics from the reference's flanking columns now live on Page 2
// (sparkline grid, also added round 40).
struct OverviewPage {
    lv_obj_t* root;
    lv_obj_t* compass;                   // lv_meter — outer dial + ticks + needles
    lv_meter_indicator_t* awa_needle;    // red — apparent wind
    lv_meter_indicator_t* ref_needle;    // green — bow / 0° reference
    lv_meter_indicator_t* port_sector;   // red close-hauled sector (top-left)
    lv_meter_indicator_t* stbd_sector;   // green close-hauled sector (top-right)

    lv_obj_t* spd_big_lbl;               // big BOAT SPD inside the speed circle
    lv_obj_t* hdg_box_lbl;               // "000" inside the heading box at top
    // Static decoration — built once, never updated; no handles needed.
};
OverviewPage overview;

// ---- Data page (round 40 — sparkline grid) ----
//
// 3×3 grid of cells inside the round panel's inscribed area. Each cell
// shows: a small grey label, a large white current value, and a 5-min
// history sparkline at the bottom. Layout is metric-symmetric so the
// most-watched values (boat speed, wind angle, wind speed) live in the
// centre column where the eye lands first.
//
// The history buffer below is sampled at 1 Hz from tick(); each metric
// gets 300 samples = 5 minutes of trail.
constexpr size_t kHistoryLen = 300;       // 5 min @ 1 Hz
constexpr int    kGridCols   = 3;
constexpr int    kGridRows   = 3;
constexpr size_t kCellCount  = kGridCols * kGridRows;

// Stable identity for each cell so we can key history buffers and refresh
// without playing string-compare games. Order = grid order, row-major:
//   row 0 (top):    AWA   TWA    TWD
//   row 1 (mid):    AWS   BSPD   TWS
//   row 2 (bottom): HDG   SOG    VMG
enum class CellMetric : uint8_t {
    AWA  = 0, TWA  = 1, TWD = 2,
    AWS  = 3, BSPD = 4, TWS = 5,
    HDG  = 6, SOG  = 7, VMG = 8,
};
static_assert(static_cast<size_t>(CellMetric::VMG) + 1 == kCellCount,
              "CellMetric layout must cover all grid cells");

struct DataCell {
    lv_obj_t*  root;
    lv_obj_t*  title_lbl;     // small grey title (e.g. "AWA  °")
    lv_obj_t*  value_lbl;     // large white value (e.g. "-27")
    lv_obj_t*  chart;         // lv_chart with one line series
    lv_chart_series_t* series;
};

struct DataPage {
    lv_obj_t* root;
    DataCell  cells[kCellCount];
};
DataPage data_pg;

// One ring buffer per cell. NaN means "no data yet". Newest sample sits at
// (head-1) mod kHistoryLen. Updated by g_state-snapshot reads in tick(),
// not by the NMEA bridge directly — so we get a clean 1 Hz timeline even
// if the bus is bursty or quiet.
struct MetricHistory {
    float   values[kHistoryLen];
    size_t  head    = 0;
    bool    filled  = false;

    MetricHistory() {
        for (auto& v : values) v = NAN;
    }
    void push(float v) {
        values[head] = v;
        head = (head + 1) % kHistoryLen;
        if (head == 0) filled = true;
    }
    // Number of valid samples currently in the buffer.
    size_t count() const { return filled ? kHistoryLen : head; }
    // Read sample i (0=oldest, count-1=newest) into *out. Returns false
    // if i is out of range.
    bool at(size_t i, float* out) const {
        const size_t n = count();
        if (i >= n || out == nullptr) return false;
        const size_t base = filled ? head : 0;
        *out = values[(base + i) % kHistoryLen];
        return true;
    }
};
MetricHistory g_history[kCellCount];

// ---- Debug page ----
struct DebugPage {
    lv_obj_t* root;
    lv_obj_t* header_lbl;               // "PGN LOG (N)"
    lv_obj_t* body_lbl;                 // recent entries, newest first
};
DebugPage debug_pg;

lv_obj_t* pages[kNumPages] = { nullptr, nullptr, nullptr };
int       current_page = 0;

// Round 39: tap-based page navigation — requested after round 38 confirmed
// touch coords arrive cleanly but LVGL swipe-gesture detection only fired
// reliably right-to-left.
//
// Why we abandoned swipe nav:
//   - LVGL's gesture detector requires many rapid intermediate samples
//     between press and release; our indev poll runs once per LVGL tick
//     (≈30 ms) so a flick across the screen produces only 3-5 samples,
//     which is on the edge of LV_INDEV_DEF_GESTURE_MIN_VELOCITY threshold.
//   - The CST820's onboard gesture detection (register 0x01 = gesture
//     byte) reports 0x00 in every read in the round-38 monitor log, so
//     the chip itself is not generating swipe codes — that register is
//     gated behind register 0xEC ("MotionMask") which defaults to 0x00.
//     Could enable it with a 0x03 write but tap is a better UX anyway.
//   - On a boat with motion + gloves, taps are far more reliable than
//     swipes. Single tap is the universally-correct gesture for "next".
//
// Mechanism: touchReadCb (round 38) tracks press/release edges and, on a
// release that's quick (< kTapMaxMs) and small-motion (< kTapMaxMovePx),
// sets g_pending_page_step to ±1. tick() consumes that flag between LVGL
// timer ticks (so we never call lv_scr_load from inside an indev cb,
// which would re-enter LVGL).
//
// The split is left-half = previous page, right-half = next page. With 3
// pages this gives full bidirectional navigation in one tap.
constexpr uint32_t kTapMaxMs       = 500;
constexpr int32_t  kTapMaxMovePx   = 40;
constexpr int32_t  kTapMaxMoveSqPx = kTapMaxMovePx * kTapMaxMovePx;

// -1 = go to previous page, +1 = next page, 0 = no pending change.
// Volatile because it's written from the indev callback (called from
// lv_timer_handler) and read from tick() which is a different call site,
// and we want the read to always see the latest write.
volatile int8_t g_pending_page_step = 0;

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

// Build the full-screen compass instrument that fills the Overview page.
//
// Z-order (back to front, which equals child-creation order in LVGL):
//   1. Outer dial — light-grey ring with tick marks + degree labels.
//      Implemented as an lv_meter widget with a grey background.
//   2. Red/green close-hauled sectors at the top of the dial (lv_meter
//      arc indicators). Fixed at ±20° around the bow for v1.
//   3. AWA red needle + green reference needle.
//   4. Inner black disc — covers the centre, leaving the tick ring
//      visible. Positioned as a child of the meter so it draws after
//      the meter's tick marks AND its needles → effectively crops the
//      needles to "outside the boat" on screen, matching how a real
//      B&G display has the needle pivot hidden under the boat.
//   5. Boat-outline silhouette (lv_line) inside the inner disc.
//   6. White-bordered speed circle, holding the BOAT-SPD label.
//   7. Heading box "000" at the top of the inner disc.
//   8. "°M" magnetic indicator at top-right of the inner disc.
//   9. "Var: 16°W" label at bottom-left of the inner disc.
void buildOverviewPage() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleScreen(scr);
    overview.root = scr;

    // ----- (1) Outer dial -----
    //
    // 460×460 fits inside the inscribed-square area of the round 480×480
    // panel with ~10 px clearance to the bezel. The widget background is
    // light grey; tick marks render in dark grey/black on top, and the
    // inner-disc child (step 4) covers everything inside radius ~140 so
    // we end up with a visible grey *annulus* of width ~90 px — enough for
    // the major-tick degree labels.
    constexpr int kCompassSize    = 460;
    constexpr int kInnerDiscSize  = 320;   // inner black disc diameter
    constexpr int kSpdCircleSize  = 120;   // white-bordered speed circle
    constexpr int kHdgBoxW        = 64;
    constexpr int kHdgBoxH        = 30;

    lv_obj_t* compass = lv_meter_create(scr);
    lv_obj_set_size(compass, kCompassSize, kCompassSize);
    lv_obj_align(compass, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(compass,
                              lv_palette_lighten(LV_PALETTE_GREY, 2),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(compass, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(compass, 0, LV_PART_MAIN);
    lv_obj_clear_flag(compass, LV_OBJ_FLAG_SCROLLABLE);

    lv_meter_scale_t* scale = lv_meter_add_scale(compass);
    // 73 ticks at 5° intervals, every 6th major (every 30°). Labels at
    // distance 18 (in pixels from the major-tick inner end) put the
    // numerals on the grey annulus, not on the inner black disc.
    lv_meter_set_scale_ticks(compass, scale, 73, 1, 6, lv_color_black());
    lv_meter_set_scale_major_ticks(compass, scale, 6, 2, 12,
                                   lv_color_black(), 18);
    // Range 0..360, full sweep, rotated so 0 sits at the top.
    lv_meter_set_scale_range(compass, scale, 0, 360, 360, 270);
    overview.compass = compass;

    // ----- (2) Close-hauled sectors at top -----
    //
    // Width 16 px, drawn on the inside of the tick ring. Red on port
    // (340..360°), green on starboard (0..20°). For v1 these are pinned;
    // a future round can make them follow TWA so the no-go zone moves.
    overview.port_sector = lv_meter_add_arc(
        compass, scale, 18, lv_palette_main(LV_PALETTE_RED), -22);
    lv_meter_set_indicator_start_value(compass, overview.port_sector, 340);
    lv_meter_set_indicator_end_value  (compass, overview.port_sector, 360);

    overview.stbd_sector = lv_meter_add_arc(
        compass, scale, 18, lv_palette_main(LV_PALETTE_GREEN), -22);
    lv_meter_set_indicator_start_value(compass, overview.stbd_sector, 0);
    lv_meter_set_indicator_end_value  (compass, overview.stbd_sector, 20);

    // ----- (3) Needles -----
    //
    // Green reference (bow / 0°) drawn first so the red AWA needle sits
    // on top when both align. r_mod = -22 keeps the needle tip 22 px shy
    // of the tick marks.
    overview.ref_needle = lv_meter_add_needle_line(
        compass, scale, 3, lv_palette_main(LV_PALETTE_GREEN), -28);
    lv_meter_set_indicator_value(compass, overview.ref_needle, 0);

    overview.awa_needle = lv_meter_add_needle_line(
        compass, scale, 4, lv_palette_main(LV_PALETTE_RED), -28);
    lv_meter_set_indicator_value(compass, overview.awa_needle, 0);

    // ----- (4) Inner black disc -----
    //
    // Child of the meter so it renders AFTER the meter's tick + needle
    // draw passes. The inner radius (~160 px) hides the inner half of
    // both needles — exactly the visual we want from the reference, where
    // the needles appear to emerge from the rim of the boat instead of
    // pivoting at the centre.
    lv_obj_t* inner = lv_obj_create(compass);
    lv_obj_set_size(inner, kInnerDiscSize, kInnerDiscSize);
    lv_obj_align(inner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(inner, kInnerDiscSize / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(inner, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(inner, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(inner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);

    // ----- (5) Boat-outline silhouette -----
    //
    // 8-point polyline shaped like a stylised hull: pointed bow, broad
    // shoulders, gently tapered stern. Drawn inside the inner disc so
    // its coordinates are relative to the disc's top-left corner, not
    // the screen.
    //
    // Reference image shows the hull centred horizontally and slightly
    // below the inner disc's geometric centre, leaving room for the
    // heading box at the top. We use the same proportions: hull is
    // 130 px wide, 240 px tall, centred at (160, 130) within the disc.
    static const lv_point_t hull_pts[] = {
        {160,  10},   // bow tip
        {200,  60},   // bow shoulder
        {220, 120},   // beam (port side, by sailor convention; we don't
                      // care about port/starboard symmetry visually)
        {220, 200},   // mid-aft
        {200, 250},   // stern shoulder
        {160, 270},   // transom centre
        {120, 250},   // stern shoulder (mirror)
        {100, 200},
        {100, 120},
        {120,  60},
        {160,  10},   // close back to the bow
    };
    lv_obj_t* hull = lv_line_create(inner);
    lv_line_set_points(hull, hull_pts,
                       sizeof(hull_pts) / sizeof(hull_pts[0]));
    lv_obj_set_style_line_color(hull, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_line_width(hull, 2, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(hull, true, LV_PART_MAIN);
    lv_obj_set_pos(hull, 0, 25);   // nudge down so bow sits below hdg box

    // ----- (6) White-bordered speed circle -----
    //
    // Sits at the boat's foredeck (slightly above geometric centre of
    // the hull) so the big BOAT-SPD value reads cleanly without
    // overlapping the AWA needle's tip when wind is dead-ahead.
    lv_obj_t* spd_circle = lv_obj_create(inner);
    lv_obj_set_size(spd_circle, kSpdCircleSize, kSpdCircleSize);
    lv_obj_align(spd_circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(spd_circle, kSpdCircleSize / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(spd_circle, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(spd_circle, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(spd_circle, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(spd_circle, 0, LV_PART_MAIN);
    lv_obj_clear_flag(spd_circle, LV_OBJ_FLAG_SCROLLABLE);

    overview.spd_big_lbl = makeLabel(spd_circle, &lv_font_montserrat_48,
                                     lv_color_white(), "--");
    lv_obj_align(overview.spd_big_lbl, LV_ALIGN_CENTER, 0, 0);

    // ----- (7) Heading box at top of inner disc -----
    lv_obj_t* hdg_box = lv_obj_create(inner);
    lv_obj_set_size(hdg_box, kHdgBoxW, kHdgBoxH);
    lv_obj_align(hdg_box, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_radius(hdg_box, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hdg_box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdg_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(hdg_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(hdg_box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hdg_box, LV_OBJ_FLAG_SCROLLABLE);

    overview.hdg_box_lbl = makeLabel(hdg_box, &lv_font_montserrat_20,
                                     lv_color_black(), "000");
    lv_obj_align(overview.hdg_box_lbl, LV_ALIGN_CENTER, 0, 0);

    // ----- (8) "°M" magnetic indicator (top-right) -----
    {
        lv_obj_t* m = makeLabel(inner, &lv_font_montserrat_16,
                                lv_palette_main(LV_PALETTE_GREY),
                                "\xC2\xB0M");
        lv_obj_align(m, LV_ALIGN_TOP_RIGHT, -22, 14);
    }

    // ----- (9) "Var: 16°W" label (bottom-left) -----
    {
        lv_obj_t* var_lbl = makeLabel(inner, &lv_font_montserrat_14,
                                      lv_palette_main(LV_PALETTE_GREY),
                                      "Var: 16\xC2\xB0W");
        lv_obj_align(var_lbl, LV_ALIGN_BOTTOM_LEFT, 22, -14);
    }
}

// Build one cell of the sparkline grid. Each cell is a 130×130 dark tile
// with three layers of content stacked vertically:
//   row 0:  small grey title  (e.g. "AWA  °")
//   row 1:  large white value (e.g. "-27")
//   row 2:  thin sparkline showing the last 5 minutes of this metric
DataCell buildDataCell(lv_obj_t* parent, int dx, int dy, const char* title) {
    DataCell c{};
    constexpr int kCellW = 130;
    constexpr int kCellH = 130;

    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_set_size(root, kCellW, kCellH);
    lv_obj_align(root, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_style_radius(root, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(root,
                                  lv_palette_darken(LV_PALETTE_GREY, 3),
                                  LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 4, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    c.title_lbl = makeLabel(root, &lv_font_montserrat_14,
                            lv_palette_main(LV_PALETTE_GREY), title);
    lv_obj_align(c.title_lbl, LV_ALIGN_TOP_LEFT, 2, 0);

    c.value_lbl = makeLabel(root, &lv_font_montserrat_28,
                            lv_color_white(), "--");
    lv_obj_align(c.value_lbl, LV_ALIGN_TOP_MID, 0, 18);

    // Sparkline: lv_chart with a single line series, 100 points wide.
    // We resample our 300-point history down to 100 points when drawing
    // (every 3rd sample) so the chart redraw is cheap and the trace
    // fits across the cell width without crowding.
    constexpr int kChartW = kCellW - 12;
    constexpr int kChartH = 32;
    constexpr uint16_t kChartPoints = 100;
    c.chart = lv_chart_create(root);
    lv_obj_set_size(c.chart, kChartW, kChartH);
    lv_obj_align(c.chart, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_chart_set_type(c.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c.chart, kChartPoints);
    lv_chart_set_div_line_count(c.chart, 0, 0);
    lv_chart_set_update_mode(c.chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(c.chart, 0, LV_PART_INDICATOR);  // no point markers
    lv_obj_set_style_bg_opa(c.chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(c.chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c.chart, 0, LV_PART_MAIN);
    // Y range gets recomputed on every refresh from the live history,
    // but seed it so the first frame doesn't render as a flatline at 0.
    lv_chart_set_range(c.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1);

    c.series = lv_chart_add_series(c.chart,
                                   lv_palette_main(LV_PALETTE_CYAN),
                                   LV_CHART_AXIS_PRIMARY_Y);

    c.root = root;
    return c;
}

void buildDataPage() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleScreen(scr);
    data_pg.root = scr;

    // 3×3 grid centred on screen. With 130 px cells and 6 px gaps the
    // outer 3-cell footprint is 130*3 + 6*2 = 402 px wide — fits inside
    // the inscribed-square area of the round panel with comfortable
    // margins. Centre column lines up with screen centre at dx=0.
    constexpr int kPitch = 136;     // 130 px cell + 6 px gap
    constexpr int kCol[] = { -kPitch, 0, kPitch };
    constexpr int kRow[] = { -kPitch, 0, kPitch };

    auto& C = data_pg.cells;
    using M = CellMetric;
    C[(int)M::AWA ] = buildDataCell(scr, kCol[0], kRow[0], "AWA  \xC2\xB0");
    C[(int)M::TWA ] = buildDataCell(scr, kCol[1], kRow[0], "TWA  \xC2\xB0");
    C[(int)M::TWD ] = buildDataCell(scr, kCol[2], kRow[0], "TWD  \xC2\xB0");
    C[(int)M::AWS ] = buildDataCell(scr, kCol[0], kRow[1], "AWS  kn");
    C[(int)M::BSPD] = buildDataCell(scr, kCol[1], kRow[1], "BSPD kn");
    C[(int)M::TWS ] = buildDataCell(scr, kCol[2], kRow[1], "TWS  kn");
    C[(int)M::HDG ] = buildDataCell(scr, kCol[0], kRow[2], "HDG  \xC2\xB0");
    C[(int)M::SOG ] = buildDataCell(scr, kCol[1], kRow[2], "SOG  kn");
    C[(int)M::VMG ] = buildDataCell(scr, kCol[2], kRow[2], "VMG  kn");
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

// --- Page navigation -------------------------------------------------------
//
// Round 39: tap-based instead of swipe-based. See the comment block by
// g_pending_page_step for why. The actual edge-detection lives in
// touchReadCb; this function applies the queued change from tick() so
// lv_scr_load happens on the LVGL main thread, not from inside an indev
// callback.
void applyPendingPageChange() {
    const int8_t step = g_pending_page_step;
    if (step == 0) return;
    g_pending_page_step = 0;
    if (step > 0) current_page = (current_page + 1) % kNumPages;
    else          current_page = (current_page + kNumPages - 1) % kNumPages;
    lv_scr_load(pages[current_page]);
}

// --- Refresh ---------------------------------------------------------------

// True-wind direction: derive from heading + true-wind angle when both are
// present. TWA is +/- relative to the bow; TWD is degrees-true 0..360.
double computeTwd(double heading_true_deg, double twa) {
    if (isnan(heading_true_deg) || isnan(twa)) return NAN;
    double d = heading_true_deg + twa;
    while (d <    0.0) d += 360.0;
    while (d >= 360.0) d -= 360.0;
    return d;
}

// Velocity made good: best-effort estimate from STW + TWA when nothing on
// the bus is publishing it directly. cos(TWA) where TWA is in degrees.
double computeVmg(double stw, double twa) {
    if (isnan(stw) || isnan(twa)) return NAN;
    return stw * cos(twa * M_PI / 180.0);
}

void refreshOverview(const Instruments& s) {
    // Wind needle: AWA is -180..180 with + starboard. Meter scale is 0..360
    // with 0=bow, so shift by +360 when negative.
    if (!isnan(s.awa)) {
        double deg = s.awa < 0 ? s.awa + 360.0 : s.awa;
        lv_meter_set_indicator_value(overview.compass,
                                     overview.awa_needle,
                                     static_cast<int32_t>(deg));
    }
    // Reference needle stays pinned to 0° (bow).
    lv_meter_set_indicator_value(overview.compass, overview.ref_needle, 0);

    // Big BOAT-SPD inside the speed circle — prefer STW (paddlewheel)
    // since the reference shows boat-through-water speed; fall back to
    // SOG when STW is missing.
    const double boat_spd = !isnan(s.stw) ? s.stw : s.sog;
    if (isnan(boat_spd)) {
        lv_label_set_text(overview.spd_big_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", boat_spd);
        lv_label_set_text(overview.spd_big_lbl, buf);
    }

    // Heading box at the top of the inner disc — three-digit zero-padded
    // degrees (000..359), or "---" if no fix yet.
    if (isnan(s.heading_true_deg)) {
        lv_label_set_text(overview.hdg_box_lbl, "---");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%03.0f", s.heading_true_deg);
        lv_label_set_text(overview.hdg_box_lbl, buf);
    }
}

// Pull the current value of one cell's metric out of the BoatState
// snapshot. Returns NaN if the metric isn't available yet.
double metricValue(CellMetric m, const Instruments& s) {
    switch (m) {
        case CellMetric::AWA:  return s.awa;
        case CellMetric::AWS:  return s.aws;
        case CellMetric::TWA:  return s.twa;
        case CellMetric::TWS:  return s.tws;
        case CellMetric::BSPD: return !isnan(s.stw) ? s.stw : s.sog;
        case CellMetric::SOG:  return s.sog;
        case CellMetric::HDG:  return s.heading_true_deg;
        case CellMetric::TWD:  return computeTwd(s.heading_true_deg, s.twa);
        case CellMetric::VMG:  return computeVmg(s.stw, s.twa);
    }
    return NAN;
}

// Format string for a metric's value label. Angles are zero-padded
// integer degrees (HDG/TWD are 0..360, AWA/TWA are signed); speeds are
// one decimal knot.
const char* metricFmt(CellMetric m) {
    switch (m) {
        case CellMetric::AWA:  return "%+.0f";
        case CellMetric::AWS:  return "%.1f";
        case CellMetric::TWA:  return "%+.0f";
        case CellMetric::TWS:  return "%.1f";
        case CellMetric::BSPD: return "%.1f";
        case CellMetric::SOG:  return "%.1f";
        case CellMetric::HDG:  return "%03.0f";
        case CellMetric::TWD:  return "%03.0f";
        case CellMetric::VMG:  return "%.1f";
    }
    return "%.1f";
}

// Refresh one cell's value label and sparkline trace from its history
// buffer. Y-axis range auto-scales to (min, max) of the visible window
// with a tiny padding so a flat trace doesn't render right at the
// border. Empty history (all NaN) leaves the trace cleared.
void refreshDataCell(CellMetric m, const Instruments& s) {
    DataCell& c = data_pg.cells[static_cast<size_t>(m)];
    const double v = metricValue(m, s);

    if (isnan(v)) {
        lv_label_set_text(c.value_lbl, "--");
    } else {
        char buf[12];
        snprintf(buf, sizeof(buf), metricFmt(m), v);
        lv_label_set_text(c.value_lbl, buf);
    }

    // Resample the 300-sample history down to 100 chart points (every
    // 3rd sample). Track min/max for axis auto-scaling. Missing samples
    // (NaN) become LV_CHART_POINT_NONE so the line drops.
    const MetricHistory& h = g_history[static_cast<size_t>(m)];
    const size_t n_hist = h.count();
    constexpr uint16_t kChartPoints = 100;
    constexpr size_t kStride = kHistoryLen / kChartPoints;  // = 3

    float vmin = INFINITY;
    float vmax = -INFINITY;

    // Walk the history newest-first and write into the chart left-to-right
    // so the newest sample lands at the right edge of the trace.
    for (uint16_t i = 0; i < kChartPoints; i++) {
        // history index for chart slot i: oldest sample on the left.
        const size_t hist_i = (n_hist >= kStride * (kChartPoints - i))
            ? (n_hist - kStride * (kChartPoints - i))
            : SIZE_MAX;
        float sample = NAN;
        if (hist_i != SIZE_MAX) h.at(hist_i, &sample);
        if (!isnan(sample)) {
            if (sample < vmin) vmin = sample;
            if (sample > vmax) vmax = sample;
            lv_chart_set_value_by_id(c.chart, c.series, i,
                                     static_cast<lv_coord_t>(sample));
        } else {
            lv_chart_set_value_by_id(c.chart, c.series, i,
                                     LV_CHART_POINT_NONE);
        }
    }

    if (vmin == INFINITY || vmax == -INFINITY) {
        lv_chart_set_range(c.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1);
    } else {
        // Pad the range a touch so flat traces don't sit on the border.
        float pad = (vmax - vmin) * 0.1f;
        if (pad < 0.5f) pad = 0.5f;
        lv_chart_set_range(c.chart, LV_CHART_AXIS_PRIMARY_Y,
                           static_cast<lv_coord_t>(vmin - pad),
                           static_cast<lv_coord_t>(vmax + pad));
    }
    lv_chart_refresh(c.chart);
}

void refreshData(const Instruments& s, size_t /*ais_count*/) {
    for (size_t i = 0; i < kCellCount; i++) {
        refreshDataCell(static_cast<CellMetric>(i), s);
    }
}

// Push the current snapshot into every metric history buffer. Called
// once per second from tick() — bounded rate independent of how often
// the bus or simulator updates BoatState.
void recordHistorySnapshot(const Instruments& s) {
    for (size_t i = 0; i < kCellCount; i++) {
        const double v = metricValue(static_cast<CellMetric>(i), s);
        g_history[i].push(static_cast<float>(v));   // NaN passes through
    }
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

// --- LVGL input device (CST820 touch) -------------------------------------

// Read one touch sample from the CST820 and feed it back to LVGL.
//
// Coordinate rotation: flushCb does an in-place 180° rotation on every
// pixel buffer (so what LVGL renders as "(0, 0) top-left" actually ends up
// at the panel's bottom-right pixel). The CST820 returns coordinates in
// the panel's native frame, so we need to apply the inverse rotation here:
// (x, y) -> (W-1-x, H-1-y). That matches the rotation in flushCb so a tap
// on the visually-top-left tile ends up at LVGL's (small, small) instead
// of at the screen's bottom-right.
//
// LVGL polls this from lv_timer_handler() at the indev refresh period
// (default 30 ms). g_touch.read() over 100 kHz I2C is ~800 µs — cheap
// enough that we don't need to gate it on the TP_INT line.
void touchReadCb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    static int16_t last_x   = 0;
    static int16_t last_y   = 0;
    static int16_t press_x  = 0;
    static int16_t press_y  = 0;
    static uint32_t press_ms = 0;
    static bool was_pressed  = false;

    uint16_t raw_x = 0, raw_y = 0;
    const bool pressed = g_touch.read(&raw_x, &raw_y);
    if (pressed) {
        last_x = static_cast<int16_t>(DISPLAY_WIDTH  - 1 - raw_x);
        last_y = static_cast<int16_t>(DISPLAY_HEIGHT - 1 - raw_y);
        if (!was_pressed) {
            // Rising edge — record where the press started.
            press_x  = last_x;
            press_y  = last_y;
            press_ms = millis();
        }
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        // Falling edge: was_pressed=true, now released. Decide whether the
        // gesture qualifies as a tap; if so, queue a page change for tick()
        // to apply (we do NOT call lv_scr_load here — that would re-enter
        // LVGL since this read_cb is called from lv_timer_handler).
        if (was_pressed) {
            const uint32_t held_ms = millis() - press_ms;
            const int32_t dx = static_cast<int32_t>(last_x) - press_x;
            const int32_t dy = static_cast<int32_t>(last_y) - press_y;
            const int32_t dist2 = dx * dx + dy * dy;
            if (held_ms < kTapMaxMs && dist2 < kTapMaxMoveSqPx) {
                g_pending_page_step =
                    (last_x < (DISPLAY_WIDTH / 2)) ? -1 : +1;
            }
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }
    // LVGL expects a coordinate even on release events (it uses it to pin
    // the release point against the last move event).
    data->point.x = last_x;
    data->point.y = last_y;
    was_pressed = pressed;
}

// --- Display driver glue ---------------------------------------------------

// LVGL -> ST7701 glue. LVGL hands us a partial-screen rectangle of RGB565
// pixels (LV_COLOR_DEPTH=16, LV_COLOR_16_SWAP=1); we forward it to the panel.
//
// Round 32: apply a 180° rotation in software. Rounds 30/31 showed that
// ST7701's MADCTL bits don't affect the displayed image when the chip is
// driven through the 16-bit RGB parallel interface (host pixels bypass
// GRAM), so the Waveshare module's physical orientation leaves LVGL
// rendering upside-down. LVGL's own lv_disp_set_rotation(LV_DISP_ROT_180)
// requires a full-framebuffer draw_buf, but we use a 40-row partial buffer
// for PSRAM budget reasons — so we rotate here instead: reverse the pixel
// buffer in place (180° == reading the rectangle end-to-start) and flip
// the destination rectangle to (W-1-x2, H-1-y2, W-1-x1, H-1-y1). This is
// an O(N) in-place swap, negligible next to the existing DMA write.
//
// Round 35: gate every drawBitmap on vsync to stop the side-to-side
// shimmer the user reported on IMG_1907.MOV. esp_lcd_panel_rgb in IDF
// 4.4.7 keeps a single PSRAM framebuffer that DMA scans continuously; if
// drawBitmap (a memcpy into that same buffer) starts at an arbitrary
// moment, the panel renders a mix of old + new rows for one or two
// frames per flush. waitForVsync() blocks until the RGB peripheral has
// finished shifting the previous frame and is in vblank, giving the
// memcpy maximum runway to stay ahead of the next row-0 scan. Cost is
// ~17 ms worst case (one frame at 58 Hz), well within our LVGL budget;
// since LVGL only flushes when something is dirty and we throttle data
// refreshes to 10 Hz in tick(), the typical wait is much shorter.
void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    const int32_t n = w * h;

    // Reverse pixel buffer in place: pixel (x, y) in the old rect moves to
    // (w-1-x, h-1-y) in the rotated rect, which is exactly position
    // n-1-(y*w+x). So swap i with n-1-i for the first half.
    for (int32_t i = 0, j = n - 1; i < j; ++i, --j) {
        lv_color_t tmp = color_p[i];
        color_p[i] = color_p[j];
        color_p[j] = tmp;
    }

    const int32_t x1 = DISPLAY_WIDTH  - 1 - area->x2;
    const int32_t y1 = DISPLAY_HEIGHT - 1 - area->y2;
    const int32_t x2 = DISPLAY_WIDTH  - 1 - area->x1;
    const int32_t y2 = DISPLAY_HEIGHT - 1 - area->y1;

    g_panel.waitForVsync();
    g_panel.drawBitmap(x1, y1, x2, y2, color_p);
    lv_disp_flush_ready(drv);
}

}  // namespace

// ---------------------------------------------------------------------------

void begin(BoatState& state) {
    g_state = &state;

    // Each step logs so a crash pinpoints the exact stage. We use log_i()
    // rather than Serial — Arduino's HWCDC-backed Serial silently drops
    // writes when the host-side CDC endpoint isn't actively reading (e.g.
    // during USB re-enumeration after reset), which was eating every one
    // of these step logs. log_i() goes through ESP-IDF's lower-level
    // console path which is not gated on CDC-connected state.
    auto step = [](const char* tag) { log_i("[ui] %s", tag); };

    // Bring up I2C, but with a pin-swap fallback: Waveshare has shipped
    // multiple revisions of this board that disagree on which GPIO is SDA
    // and which is SCL, and we can't tell from software which rev we're on.
    // So: try the documented assignment first, scan; if zero devices ACK,
    // swap and scan again. Whichever ordering produces devices is the right
    // one and we use it for the rest of boot. This costs us nothing when
    // the primary ordering is correct (the scan was going to run anyway)
    // and makes the "wrong board rev" failure mode instantly self-healing.
    auto scanBus = []() {
        int found = 0;
        for (uint8_t addr = 1; addr < 127; ++addr) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) ++found;
        }
        return found;
    };

    // Before we even ask the Wire driver to touch the bus, sniff the idle
    // state of the SDA/SCL candidate pins as plain inputs with the internal
    // pull-ups engaged. Three outcomes, and each one gives us a clear next
    // step:
    //   (a) both idle HIGH  → bus is electrically healthy. Any device on it
    //                          can't be that far away — either we have the
    //                          wrong pin pair or one specific device is dead.
    //   (b) either stuck LOW → someone's shorting the rail to ground. Usual
    //                          culprits: wrong pins (the GPIO we picked is
    //                          being driven by something else — e.g. another
    //                          peripheral hard-tied to the same net), a held
    //                          reset, or no 3V3 on the I2C side of the board.
    //   (c) pins float mid-rail → internal pull-up not engaging. Rare on S3,
    //                          but possible if the pin is strapping-restricted
    //                          or if a weak external pull-down exists.
    //
    // This runs BEFORE Wire.begin() so we see the truly-idle state, not a
    // state shaped by the I2C controller's own driver. We put the pins back
    // in INPUT afterwards so Wire.begin() owns them cleanly.
    auto probeIdle = [](int sda, int scl) {
        pinMode(sda, INPUT_PULLUP);
        pinMode(scl, INPUT_PULLUP);
        delayMicroseconds(200);   // give the pull-ups time to charge the line
        const int sda_hi = digitalRead(sda);
        const int scl_hi = digitalRead(scl);
        log_i("[ui] i2c: idle-state probe (SDA=GPIO%d, SCL=GPIO%d) → "
              "SDA=%s, SCL=%s",
              sda, scl,
              sda_hi ? "HIGH (ok)" : "LOW (bus dead / shorted?)",
              scl_hi ? "HIGH (ok)" : "LOW (bus dead / shorted?)");
        // Release so the I2C driver can take over ownership.
        pinMode(sda, INPUT);
        pinMode(scl, INPUT);
    };

    // Wait for USB CDC to stabilize before spraying diagnostics — otherwise
    // the early log lines get eaten during enumeration after reset.
    delay(1000);

    // Chip identity banner. Confirms module variant so we can cross-check
    // which ESP32-S3 SKU (N16R8? N8R2?) is actually on the board. Some GPIOs
    // are only internally-tied-off on specific PSRAM-equipped variants.
    log_i("[ui] ===== CHIP IDENTITY =====");
    log_i("[ui] chip:           %s rev %d, %d core(s)",
          ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
    log_i("[ui] cpu freq:       %u MHz", (unsigned)ESP.getCpuFreqMHz());
    log_i("[ui] flash size:     %u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    log_i("[ui] psram size:     %u MB", (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
    log_i("[ui] sdk version:    %s", ESP.getSdkVersion());
    log_i("[ui] ===== END CHIP IDENTITY =====");
    delay(100);

    // Broad GPIO idle-state scan. Previous rounds only probed three pairs we
    // guessed at (8/9, 10/11, 6/7) — all either electrically fine-but-silent
    // or stuck. Now we scan EVERY safe GPIO and report idle state. The
    // signature to look for:
    //   HIGH with internal pull-up = pin is free, nothing connected.
    //   LOW  despite internal pull-up = pin is being actively pulled down by
    //        a peripheral → that's where a device (or a tied strap) lives.
    // Pairs of LOW pins in a plausible layout are strong candidates for the
    // real I2C bus on this specific board rev.
    //
    // Pins we deliberately SKIP on an ESP32-S3-WROOM-1 N16R8:
    //   26..32 — SPI flash (internal).
    //   33..37 — octal PSRAM (internal on N16R8). Touching breaks PSRAM.
    //   43, 44 — UART0 tx/rx (debug console routing; safer to leave alone).
    // Everything else is either general-purpose or strap-only and safe to
    // read as INPUT_PULLUP.
    static const int kScanPins[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21,
        38, 39, 40, 41, 42, 45, 46, 47, 48,
    };
    log_i("[ui] ===== GPIO IDLE-STATE SCAN =====");
    delay(50);
    for (size_t i = 0; i < sizeof(kScanPins) / sizeof(kScanPins[0]); ++i) {
        const int p = kScanPins[i];
        pinMode(p, INPUT_PULLUP);
        delayMicroseconds(200);
        const int hi = digitalRead(p);
        log_i("[ui] gpio %2d : %s",
              p, hi ? "HIGH (free / pulled-up)" : "LOW  (tied down — device?)");
        pinMode(p, INPUT);   // release cleanly
        // Small pacing so log_i doesn't overflow the CDC drain.
        if ((i % 4) == 3) {
            delay(30);
        }
    }
    log_i("[ui] ===== END GPIO IDLE-STATE SCAN =====");
    delay(100);

    // Idle-state sanity probe on the locked-in I2C pair. With the pivot
    // we already know SDA=15 / SCL=7 is the right answer; this probe stays
    // as a quick visual "are the lines free?" check before Wire.begin(). If
    // either line reads LOW here something is electrically wrong — usually
    // a device holding the line low because its reset isn't driven.
    log_i("[ui] ===== I2C IDLE-STATE DIAGNOSTIC =====");
    delay(100);
    probeIdle(display::I2C_PIN_SDA, display::I2C_PIN_SCL);
    delay(100);
    log_i("[ui] ===== END I2C IDLE-STATE DIAGNOSTIC =====");
    delay(100);

    // ---- one-shot I2C bus enumeration (round 11) ------------------------
    // SDA=GPIO15 / SCL=GPIO7 locked in per the round-10 scan (see
    // display_pins.h). We still dump the full 127-address scan at boot so
    // every bring-up log captures "here's what was on the bus this time" —
    // cheap (~50 ms), makes failure modes ("expander missing!") jump out.
    //
    // Known bus fingerprint for this board rev:
    //   0x15  CST820 touch
    //   0x20  TCA9554 I/O expander
    //   0x51  PCF85063 RTC
    //   0x6B  (probably QMI8658 IMU — round-10 discovery, not yet used)
    //   0x7E  (unknown — may be a real device or a hal-i2c artefact)
    log_i("[ui] ===== I2C BUS ENUMERATION (SDA=GPIO%d, SCL=GPIO%d) =====",
          display::I2C_PIN_SDA, display::I2C_PIN_SCL);
    delay(50);
    Wire.end();
    Wire.begin(display::I2C_PIN_SDA, display::I2C_PIN_SCL, display::I2C_FREQ_HZ);
    int enum_hits = 0;
    for (uint8_t addr = 1; addr < 127; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            log_i("[ui]   ACK 0x%02X", addr);
            ++enum_hits;
        }
        if ((addr & 0x1F) == 0x1F) delay(5);
    }
    log_i("[ui] bus enumeration: %d device(s) ACKed", enum_hits);
    if (enum_hits < 3) {
        log_e("[ui] expected at least 0x15/0x20/0x51 — bus may be mis-wired "
              "or a reset line is held. Check display_pins.h.");
    }
    Wire.end();
    log_i("[ui] ===== END I2C BUS ENUMERATION =====");
    delay(50);

    step("Wire.begin() [primary from display_pins.h]");
    Wire.begin(display::I2C_PIN_SDA, display::I2C_PIN_SCL, display::I2C_FREQ_HZ);
    int device_count = scanBus();
    log_i("[ui] i2c: primary ordering found %d device(s)", device_count);

    if (device_count == 0) {
        log_w("[ui] i2c: primary ordering saw zero devices, trying swapped "
              "pins (SDA=%d, SCL=%d)",
              display::I2C_PIN_SCL, display::I2C_PIN_SDA);
        Wire.end();
        Wire.begin(display::I2C_PIN_SCL, display::I2C_PIN_SDA, display::I2C_FREQ_HZ);
        device_count = scanBus();
        log_i("[ui] i2c: swapped ordering found %d device(s)", device_count);
        if (device_count == 0) {
            log_e("[ui] i2c: still zero devices after swapping pins. "
                  "Check 3V3 rail on the I2C bus, external pull-up resistors, "
                  "and whether this is actually an ESP32-S3-Touch-LCD-2.1.");
        } else {
            log_i("[ui] i2c: SWAP WORKED — update display_pins.h to "
                  "I2C_PIN_SDA=%d, I2C_PIN_SCL=%d permanently.",
                  display::I2C_PIN_SCL, display::I2C_PIN_SDA);
        }
    }

    step("TCA9554.begin()");
    if (!g_expander.begin()) {
        log_e("[ui] TCA9554 not responding on I2C - backlight + resets "
              "won't be driven. Check I2C pins / board rev.");
    }

    // ---- Round-12 TCA9554 bit-mapping diagnostic removed in round 13 ----
    // The factory firmware that shipped on this board showed a working
    // settings UI on first power-up, so the hardware path from TCA9554 →
    // backlight → panel is fine. Brute-forcing TCA9554 bits without a valid
    // RGB pixel stream doesn't help: the LC layer is opaque until the
    // ST7701 is receiving live pixel data, so no phase will ever light up
    // visibly regardless of which bit is LCD_BL. The bit-mapping was also
    // mostly resolved indirectly — the phase-B/J beeps identified IO7 as
    // a piezo buzzer, and the LovyanGFX community config confirmed
    // IO1 = LCD_RST, IO2 = TP_RST, IO3 = LCD_CS. See display_pins.h for
    // the full mapping. Round 14 will drive that mapping properly from
    // inside the new ST7701 panel driver.

    step("ST7701.begin()");
    if (!g_panel.begin(g_expander)) {
        log_e("[ui] ST7701 bring-up failed - continuing without pixels; "
              "LVGL will still tick.");
    }

    step("CST820.begin()");
    if (!g_touch.begin(g_expander)) {
        log_w("[ui] CST820 bring-up failed - touch input disabled this boot. "
              "Swipe nav will not work; check I2C / TP_RST wiring.");
    }
    // Round 34: the 5-phase RGB+W+K boot sanity bar from rounds 28-33 is
    // removed. Display bring-up is complete (round 30 nailed colours +
    // timings via Waveshare's verbatim init, round 32 flipped the image
    // right-side up via in-place 180° rotation in flushCb, round 33
    // killed the wide brightness bands by switching ST7701 0xC2 byte 0
    // from 0x07 to 0x37 / inversion-mode bits 00 → 11). Boat use wants
    // immediate data on power-up, not 25 s of solid-colour fills.
    // Sanity-bar history is preserved in git: revert 8c... if needed.

    step("lv_init()");
    lv_init();

    step("ps_malloc framebuffers");
    // Round 34: full-frame double-buffer (was 40-row partial buffers).
    //
    // The 40-row partial buffer (DISPLAY_WIDTH * 40 = 19,200 px each, two
    // of them) split a full-screen redraw across 12 flush calls. On an
    // RGB-interface panel like the ST7701 the peripheral is continuously
    // scanning the framebuffer at ~58 Hz; with 12 separate writes per
    // refresh the panel's scan often catches the framebuffer mid-update,
    // producing visible shimmer on animated content (the round-33 IMG_1906
    // showed this on the rotating wind-compass needle, hence the user's
    // "twinkles back and forth towards 270 and 90" report).
    //
    // Full-frame double-buffer eliminates the tear: LVGL renders into one
    // 480 × 480 buffer while the previous one is being DMA'd to the panel,
    // then we flush exactly one rectangle per frame. Cost: 2 × 480 × 480 ×
    // sizeof(lv_color_t) = 2 × 460,800 = 921,600 bytes in PSRAM, plenty
    // of headroom against our 8 MB octal PSRAM budget.
    const size_t fb_px = DISPLAY_WIDTH * DISPLAY_HEIGHT;
    buf1 = static_cast<lv_color_t*>(ps_malloc(fb_px * sizeof(lv_color_t)));
    buf2 = static_cast<lv_color_t*>(ps_malloc(fb_px * sizeof(lv_color_t)));
    if (!buf1 || !buf2) {
        log_e("[ui] ps_malloc failed: buf1=%p buf2=%p (%u bytes each)",
              buf1, buf2,
              static_cast<unsigned>(fb_px * sizeof(lv_color_t)));
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, fb_px);

    step("lv_disp_drv_register");
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = DISPLAY_WIDTH;
    disp_drv.ver_res  = DISPLAY_HEIGHT;
    disp_drv.flush_cb = flushCb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Register the CST820 as an LVGL pointer input device. We do this even
    // when g_touch.begin() failed — the read_cb just returns RELEASED in
    // that case (g_touch.read() returns false because ready_ stays false),
    // so swipe nav is dead but nothing crashes. This way one corrupt boot
    // doesn't take the whole UI offline.
    step("lv_indev_drv_register (touch)");
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchReadCb;
    lv_indev_drv_register(&indev_drv);

    step("build pages");
    buildOverviewPage();
    buildDataPage();
    buildDebugPage();
    pages[0] = overview.root;
    pages[1] = data_pg.root;
    pages[2] = debug_pg.root;

    lv_scr_load(pages[0]);
    // Round 39: no LV_EVENT_GESTURE handler — tap detection lives in
    // touchReadCb and is applied via applyPendingPageChange() in tick().
    step("ready");
}

uint32_t tick() {
    // Round 35: throttle widget data refreshes to 10 Hz. The simulator
    // updates BoatState every loop iteration (sub-millisecond), and
    // refreshFromState() rewrites every label on the active page each
    // call — every changed character marks an LVGL region dirty, which
    // triggers a flushCb, which is now (round 35) a vsync-gated
    // 460 KB memcpy. Doing that on every loop tick is gratuitous: NMEA
    // 2000 instruments don't update useful information faster than
    // 5-10 Hz, and at 100 ms throttling the visual response stays well
    // under human reaction time while the LVGL flush rate drops by
    // roughly 20× compared with the round 34 build. Less flush =
    // less drawBitmap = less framebuffer churn = less opportunity for
    // the panel scanout to catch a partially-written frame. The
    // lv_timer_handler() call below still runs every iteration so
    // animations, gestures and timers stay responsive.
    static uint32_t last_data_refresh_ms = 0;
    static uint32_t last_history_ms      = 0;
    const uint32_t now = millis();
    if (now - last_data_refresh_ms >= 100) {
        last_data_refresh_ms = now;
        refreshFromState();
    }

    // Round 40: sample BoatState into the per-metric history ring buffers
    // once per second. This drives the Page 2 sparklines — kHistoryLen=300
    // samples × 1 Hz = 5 minutes of trail. Sampling here (not from the
    // NMEA bridge directly) gives us a clean uniform timeline regardless
    // of bus burstiness or sim cadence, and it keeps the chart's resample
    // stride (kStride = 3) well-defined. Runs even when Page 2 isn't
    // visible so the sparkline already has trace data when the user
    // taps over to it.
    if (g_state && now - last_history_ms >= 1000) {
        last_history_ms = now;
        recordHistorySnapshot(g_state->snapshot());
    }

    // Round 39: apply any pending page change queued by touchReadCb's
    // tap detection. We do this here (between LVGL timer ticks) instead
    // of in the indev callback because lv_scr_load() needs to dispatch
    // events on objects we'd otherwise be in the middle of polling.
    applyPendingPageChange();

    return lv_timer_handler();
}

}  // namespace ui

#endif  // DISPLAY_SAFE_MODE
