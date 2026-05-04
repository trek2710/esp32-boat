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
#include <esp_heap_caps.h>     // for heap_caps_malloc (cone-canvas buffer)
// NOTE: include real LVGL via the lvgl/ subpath. Historical reason kept in
// case we ever re-add LovyanGFX for anything: that library ships its own
// lvgl.h shim that hijacked the unqualified include. Going via the lvgl/
// subdir remains unambiguous.
#include <lvgl/lvgl.h>

#include <cmath>     // std::lround for inner-compass rotation angle
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

constexpr int kNumPages = 4;

// ---- Overview page (round 42 — second B&G reference, black dial) ----
//
// Round 42 replaces the round-40/41 design (light-grey dial, hull
// silhouette, big speed-circle on the bow) after the user pointed at a
// second B&G-style reference image and asked for that look "exactly":
//
//   * Outer dial: BLACK ring, ~60 px wide, with WHITE tick marks and
//     WHITE numerical labels at 30° intervals. Reads clockwise from
//     0° at the top.
//   * Red/green close-hauled tick markers at the top — implemented as
//     SCALE_LINES indicators that re-colour the existing tick marks in
//     the ±40° window around the bow, instead of overlaying a solid arc
//     (the reference has discrete coloured tick lines, not a filled
//     bar).
//   * Inner black disc covering the centre.
//   * Wind pointer (apparent wind direction): white-bordered fat
//     pointer, drawn as two stacked lv_meter needles — a wide white
//     needle (width 16) with a narrower navy fill (width 10) on top.
//     Reads as a triangle-ish indicator from the rim toward the
//     centre. Pivots are hidden by the inner disc.
//   * Centre stack: small white-bordered pill at the top showing
//     "x.x k DRIFT" (boat speed-through-water; "drift" matches the
//     reference's labelling), and a bigger box below showing "xx.x k
//     AWS" — apparent wind speed.
//   * Small white triangle at the bottom of the inner disc — a stylised
//     heading / reference marker pointing UP toward the bow.
//
// What got dropped vs round 41: the boat-outline polyline (the new
// reference doesn't have one), the 0° reference needle, the heading-
// box label, and the °M / Var: 16°W small text. The 9-metric
// sparkline grid on Page 2 is unchanged.
struct OverviewPage {
    lv_obj_t* root;
    lv_obj_t* compass;                       // lv_meter — outer dial + ticks
    lv_meter_indicator_t* awa_needle;        // canvas-drawn cone (needle_img) — apparent wind
    lv_meter_indicator_t* twa_needle;        // thin yellow line — true wind direction
    lv_meter_indicator_t* port_sector;       // solid red arc — port close-hauled
    lv_meter_indicator_t* stbd_sector;       // solid green arc — stbd close-hauled

    lv_obj_t* drift_value_lbl;               // STW value inside the centre circle
    lv_obj_t* aws_value_lbl;                 // AWS — left of the hull (round 52)
    lv_obj_t* bspd_value_lbl;                // boat speed — right of the hull (round 52)
};
OverviewPage overview;

// ---- Main display page (round 54) ----
//
// Round 54 dropped the round-41/52 sparkline data grid and replaced it
// with a new "Main display" page (now Page 0). The wind page becomes
// Page 1, debug Page 2, and a new simulator page Page 3 shows the raw
// sensor values being fed into BoatState.
//
// Main page layout (per the user's reference image + spec):
//   * Outer ring (FIXED): mirrored 0..180 labels each side of the bow
//     with red/green close-hauled sectors at top — same widget pattern
//     as the wind page's outer ring.
//   * Inner compass ring (ROTATES with heading): degree labels every
//     30° (030, 060, ..., 330) plus a green N marker. As the boat
//     turns, the ring rotates so the heading appears under the bow at
//     the top of the dial.
//   * Boat hull at the centre (slim 24-point polygon, same as the wind
//     page).
//   * Heading "038" label at the bow (top of the boat).
//   * BSPD numeric readout at the stern (bottom of the boat).
//   * Wide blue arrow in the centre indicating true-wind direction
//     (TWD), drawn as a needle_img cone like the wind page's AWA cone
//     but rotated to TWD instead of AWA.
//   * Heel-angle indicator on the LEFT side (placeholder line for
//     now — we don't have a heel sensor yet, value pinned to 0°).
//   * Depth readout on the RIGHT side (numeric, "x.x m").
//   * Blue "T" target triangle on the OUTER rim — indicates the set
//     course (autopilot target). Stub for now: pinned 30° clockwise
//     from heading until we have a SET command source.
struct MainPage {
    lv_obj_t* root;

    lv_obj_t* compass;                   // outer fixed ring (lv_meter)
    lv_meter_indicator_t* port_sector;
    lv_meter_indicator_t* stbd_sector;
    lv_meter_indicator_t* target_marker; // blue T triangle on outer rim

    lv_obj_t* inner_ring;                // rotating compass container
    lv_obj_t* twd_canvas_holder;         // hidden canvas backing the cone
    lv_meter_indicator_t* twd_arrow;     // blue cone for TWD

    lv_obj_t* heading_lbl;               // "038" at bow
    lv_obj_t* bspd_lbl;                  // boat speed at stern
    lv_obj_t* depth_lbl;                 // depth on right side
    lv_obj_t* heel_lbl;                  // heel angle on left side
};
MainPage main_pg;

// ---- Simulator page (round 54) ----
//
// Read-only display of the raw sensor inputs the simulator publishes
// into BoatState, plus the derived values BoatState computes from
// them. Useful for visually verifying the round-53 raw/derived split
// without scraping the serial monitor.
struct SimulatorPage {
    lv_obj_t* root;

    // Raw sensor values:
    lv_obj_t* lat_lbl;
    lv_obj_t* lon_lbl;
    lv_obj_t* sog_lbl;
    lv_obj_t* cog_lbl;
    lv_obj_t* awa_lbl;
    lv_obj_t* aws_lbl;
    lv_obj_t* hdg_m_lbl;
    lv_obj_t* var_lbl;
    lv_obj_t* stw_lbl;
    lv_obj_t* depth_lbl;

    // Derived values (computed by BoatState):
    lv_obj_t* hdg_t_lbl;
    lv_obj_t* twa_lbl;
    lv_obj_t* tws_lbl;
    lv_obj_t* twd_lbl;
    lv_obj_t* vmg_lbl;
};
SimulatorPage sim_pg;

// ---- Debug page ----
struct DebugPage {
    lv_obj_t* root;
    lv_obj_t* header_lbl;               // "PGN LOG (N)"
    lv_obj_t* body_lbl;                 // recent entries, newest first
};
DebugPage debug_pg;

lv_obj_t* pages[kNumPages] = { nullptr, nullptr, nullptr, nullptr };
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
// Round 55/57/58/60/62: SWIPE nav qualifier. A release counts as a swipe when:
//   * horizontal travel ≥ kSwipeMinPx     (deliberate sideways motion)
//   * |dx| > |dy|                          (more horizontal than vertical)
//
// Round 62 dropped the kSwipeMaxMs duration cap. The CST820 streams
// coordinates intermittently — sometimes 32 ms, sometimes 10 s — and
// our held_ms = last_real_press_ms - press_ms only counts the actively
// streaming portion, which gave wildly inconsistent values for
// objectively-similar finger gestures. Round-61 trace had a clear
// -191 px swipe rejected on held=10521 ms even though the user was
// clearly intending to swipe. If the chip reported motion, the user
// moved; that's enough to call it a swipe. The 1200 ms hold-through
// gap already keeps lazy long-presses from accidentally registering.
//
// kSwipeMaxMs is kept around for the diagnostic log line below.
constexpr uint32_t kSwipeMaxMs   = 3000;   // (informational only — see above)
// Round 63: 40 → 30. The round-62 trace had a clear left-swipe at
// dx=-31 px held=1799 ms rejected for being just under the 40-px floor.
// 30 px is still well above noise (a stationary finger jitters ≤ 4 px on
// the CST820) but covers shorter, faster swipes.
constexpr int32_t  kSwipeMinPx   = 30;

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
// See the OverviewPage doc-comment higher up for the round-42 design
// intent (black dial, white ticks/labels, tick-coloured close-hauled
// markers, white-bordered fat wind pointer, DRIFT pill + AWS box,
// heading triangle — drop the hull silhouette).
//
// Z-order (back to front = LVGL child-creation order):
//   1. Outer black dial with white minor + major ticks and white labels.
//   2. Red / green close-hauled tick lines (SCALE_LINES indicators) —
//      they re-colour the existing tick marks in the ±40° window
//      around the bow rather than drawing a solid arc.
//   3. Wind pointer: stacked needle pair (wide white below, narrower
//      navy on top) — looks like a triangular pointer outlined in
//      white.
//   4. Inner black disc — covers the centre and crops the inner half
//      of both needles (so the pointer appears to emerge from the rim).
//   5. (deleted in round 42 — was the boat-outline silhouette)
//   6. DRIFT pill at upper-mid of the disc (small) and AWS box at
//      lower-mid (big white-bordered black box).
//   7. Small white triangle at the bottom of the disc — heading marker
//      pointing toward the bow.
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
    constexpr int kCompassSize    = 460;   // outer dial diameter
    constexpr int kInnerDiscSize  = 320;   // inner black disc diameter

    lv_obj_t* compass = lv_meter_create(scr);
    lv_obj_set_size(compass, kCompassSize, kCompassSize);
    lv_obj_align(compass, LV_ALIGN_CENTER, 0, 0);
    // Round 42: dial is BLACK (was light grey) to match the second B&G
    // reference. Tick lines + tick labels are then drawn in white below.
    lv_obj_set_style_bg_color(compass, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(compass, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(compass, 0, LV_PART_MAIN);
    lv_obj_clear_flag(compass, LV_OBJ_FLAG_SCROLLABLE);
    // Round 44: hide lv_meter's auto-generated tick labels (which would
    // print 0, 30, 60, ..., 330 sequentially) by setting the tick text
    // colour to match the dial background. We then draw 12 mirrored
    // labels manually below — 0/30/60/90/120/150/180 going DOWN the
    // right side and 180/150/120/90/60/30/0 mirrored on the left,
    // matching the classic sailing-instrument relative-bearing layout.
    lv_obj_set_style_text_color(compass, lv_color_black(), LV_PART_TICKS);
    lv_obj_set_style_text_font(compass, &lv_font_montserrat_20, LV_PART_TICKS);

    lv_meter_scale_t* scale = lv_meter_add_scale(compass);
    // Round 41 (per user feedback "add more angles"): tick density doubled
    // from 73 minor ticks @ 5° (round 40) to 145 minor ticks @ 2.5°. Major
    // ticks stay at every 30° (every 12th tick) but are taller and
    // thicker so the cardinal positions still pop through the denser
    // minor field. Result reads visibly more like the B&G reference,
    // which has very fine angular resolution around the rim.
    lv_meter_set_scale_ticks(compass, scale, 145, 1, 8, lv_color_white());
    lv_meter_set_scale_major_ticks(compass, scale, 12, 3, 16,
                                   lv_color_white(), 18);
    lv_meter_set_scale_range(compass, scale, 0, 360, 360, 270);
    overview.compass = compass;

    // ----- (1b) Manual mirrored degree labels -----
    //
    // Round 44 (per user "Degree should go from 0 to 180 in both sides"):
    // place 12 lv_label widgets at 30° intervals around the dial, with
    // text mirrored across the vertical axis so both halves read 0 at
    // top, increasing through 30/60/90/120/150 to 180 at the bottom —
    // the classic relative-bearing layout used by sailing instruments.
    //
    // lv_meter's built-in labels print sequentially (0/30/.../330) and
    // can't be relabelled, so we hide them via the LV_PART_TICKS text
    // colour above and overlay these manual ones at the same radial
    // distance.
    {
        constexpr float kLabelRadius = 188.0f;
        for (int i = 0; i < 12; ++i) {
            const float angle_deg = static_cast<float>(i) * 30.0f;
            const float a = angle_deg * static_cast<float>(M_PI) / 180.0f;
            const int dx = static_cast<int>(kLabelRadius * sinf(a));
            const int dy = -static_cast<int>(kLabelRadius * cosf(a));
            const int val = (i <= 6) ? (i * 30) : ((12 - i) * 30);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", val);
            lv_obj_t* lbl = makeLabel(compass, &lv_font_montserrat_20,
                                      lv_color_white(), buf);
            lv_obj_align(lbl, LV_ALIGN_CENTER, dx, dy);
        }
    }

    // ----- (2) Close-hauled sectors at top -----
    //
    // Width 16 px, drawn on the inside of the tick ring. Red on port
    // (340..360°), green on starboard (0..20°). For v1 these are pinned;
    // a future round can make them follow TWA so the no-go zone moves.
    // Round 41 (per user feedback "Use red/green"): sectors widened from
    // ±20° to ±40° around the bow and thickened from 18 to 26 px so the
    // no-go zone reads as a dominant visual feature, matching the B&G
    // reference where the red/green arcs are the most prominent thing
    // on the dial. Port = red on the left of bow (320..360°), starboard
    // = green on the right (0..40°). For v1 these are pinned; a future
    // round will follow TWA so the no-go zone tracks the live wind.
    // Round 43: SCALE_LINES indicators (round 42 attempt) didn't paint
    // visibly on the panel — user reported "no red/green colors". Reverted
    // to solid lv_meter_add_arc bars (round-41 method, known good): width
    // 26 px, port red 320..360°, starboard green 0..40°, both pulled
    // 22 px inside the tick ring via r_mod = -22.
    // Round 44: sectors widened from 40° to 60° each side per user.
    // Port red 300..360 (left of bow, 60° of dial), starboard green
    // 0..60 (right of bow, 60° of dial). Both pulled 22 px inside the
    // tick ring via r_mod = -22.
    // Round 45 (per user "red is purple, green is light green / should be
    // dark green"): switch from LV_PALETTE_main values to explicit hex.
    // Material RED 500 (#F44336) was rendering as purple on the panel
    // and Material GREEN 500 (#4CAF50) as a wishy-washy light green.
    // 0xCC0000 (deep red) and 0x006400 (DarkGreen) are unambiguous.
    overview.port_sector = lv_meter_add_arc(
        compass, scale, 26, lv_color_hex(0xCC0000), -22);
    lv_meter_set_indicator_start_value(compass, overview.port_sector, 300);
    lv_meter_set_indicator_end_value  (compass, overview.port_sector, 360);

    overview.stbd_sector = lv_meter_add_arc(
        compass, scale, 26, lv_color_hex(0x006400), -22);
    lv_meter_set_indicator_start_value(compass, overview.stbd_sector, 0);
    lv_meter_set_indicator_end_value  (compass, overview.stbd_sector, 60);

    // ----- (3) Wind pointer (stacked needle pair) -----
    //
    // Round 44 (per user "we have a cone rather than an arrow/rod"):
    // replace the round-43 stacked rectangular needle pair with a
    // proper tapered cone. lv_meter's needle_line is rectangular, so
    // we render the cone shape into an lv_canvas once at init and then
    // register the canvas's image data as an lv_meter_add_needle_img
    // — needle_img rotates the image around its pivot which is mapped
    // to the meter centre.
    //
    // Image is 60 wide × 220 tall, TRUE_COLOR_ALPHA so the area outside
    // the triangle is transparent. Pivot at (30, 220) = bottom-centre,
    // so when the indicator value is at "0° = top", the image's tip
    // (30, 0) extends 220 px UP from the meter centre — i.e. just shy
    // of the dial rim (compass radius is 230). Cone tapers from a
    // wide base near the centre to a sharp tip at the rim. White outer
    // outline + navy fill, matching the reference image.
    // Round 45 cone tuning per user "the arrow should be larger and go
    // close to the center circle":
    //   * Image dimensions 80×180 (was 60×220) — wider visible base
    //     and the visible cone region pushed deeper toward the centre.
    //   * Pivot at (40, 179) — bottom-centre of the image, lands on the
    //     meter centre.
    //   * Visible cone occupies the top 130 px (y=0..129) of the image;
    //     the bottom 50 px are LEFT TRANSPARENT. With pivot at y=179
    //     and visible base at y=129, the visible base lands 50 px
    //     above the meter centre — about 1 px above the DRIFT circle's
    //     top edge (which is at y_compass=181), so the cone visually
    //     "ends" right at the centre circle.
    //   * Visible base 70 px wide vs round-44's 52 px → much chunkier
    //     pointer.
    // Round 46: cone shrunk substantially after IMG_1922 showed the
    // round-45 80×180 image overwhelmed the dial. New dimensions
    // 50×140 with bottom 50 px transparent give a 40 px wide visible
    // base and a 90 px tall visible cone — proportional to the dial,
    // and still tapered enough to read as a cone rather than a rod.
    constexpr int kConeW           = 50;
    constexpr int kConeH           = 140;
    constexpr int kConeVisibleBase = 90;      // last visible y row
    const size_t cone_buf_sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(kConeW, kConeH);
    lv_color_t* cone_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(cone_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (cone_buf) {
        lv_obj_t* cone_canvas = lv_canvas_create(compass);
        lv_canvas_set_buffer(cone_canvas, cone_buf, kConeW, kConeH,
                             LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(cone_canvas, lv_color_black(), LV_OPA_TRANSP);

        // White outer triangle (forms the border).
        lv_draw_rect_dsc_t outer_dsc;
        lv_draw_rect_dsc_init(&outer_dsc);
        outer_dsc.bg_color = lv_color_white();
        outer_dsc.bg_opa   = LV_OPA_COVER;
        const lv_point_t outer_tri[3] = {
            {kConeW / 2,                  0},
            {kConeW - 5, kConeVisibleBase   },
            {         5, kConeVisibleBase   },
        };
        lv_canvas_draw_polygon(cone_canvas, outer_tri, 3, &outer_dsc);

        // Navy inner triangle (3-4 px inset from the outer outline at
        // this smaller cone size — keeps a visible white border without
        // making the inner fill disappear).
        lv_draw_rect_dsc_t inner_dsc;
        lv_draw_rect_dsc_init(&inner_dsc);
        inner_dsc.bg_color = lv_color_hex(0x1A2740);
        inner_dsc.bg_opa   = LV_OPA_COVER;
        const lv_point_t inner_tri[3] = {
            {kConeW / 2,                   6},
            {kConeW - 8, kConeVisibleBase - 4},
            {         8, kConeVisibleBase - 4},
        };
        lv_canvas_draw_polygon(cone_canvas, inner_tri, 3, &inner_dsc);

        // Hide the canvas widget itself — we only want the image data.
        lv_obj_add_flag(cone_canvas, LV_OBJ_FLAG_HIDDEN);

        // Register the canvas's image as the wind needle. Pivot at
        // bottom-centre lands on the meter centre.
        overview.awa_needle = lv_meter_add_needle_img(
            compass, scale,
            lv_canvas_get_img(cone_canvas),
            kConeW / 2, kConeH - 1);
        lv_meter_set_indicator_value(compass, overview.awa_needle, 0);
    } else {
        log_e("[ui] cone canvas alloc failed (%u bytes) — wind pointer "
              "won't render this boot", static_cast<unsigned>(cone_buf_sz));
        overview.awa_needle = nullptr;
    }

    // Round 45: yellow TWA (true-wind-angle) needle. Smaller than the
    // AWA cone — a thin 6-px-wide line — and rendered as a simple
    // lv_meter_add_needle_line. Drawn AFTER the AWA cone so its tail
    // sits on top of the cone where they overlap; both rotate around
    // the meter centre with their own values.
    // Round 50 (per user "yellow line should be a triangle at the rim
    // pointing inwards"): replace the radial needle_line with a small
    // inward-pointing triangle marker that sits AT the rim. Implemented
    // as an lv_meter_add_needle_img using a canvas where:
    //   * the top 22 px hold a yellow triangle whose base spans the
    //     full image width at y=0 (the rim end) and whose apex points
    //     DOWN at y=22 (toward the centre);
    //   * the bottom 178 px are transparent — purely for the rotation
    //     pivot, since needle_img rotates around its pivot which is
    //     mapped to the meter centre.
    // When the indicator value is set to TWA, the image rotates around
    // the centre and the visible triangle ends up at the dial rim at
    // angle TWA, with its apex pointing inward.
    // Round 52 — TWA triangle "did not follow the outer edge". Cause:
    // image height was 200 px, so with pivot at the bottom landing on
    // the meter centre, the top of the image (where the triangle
    // sits) ended up at radius 199 — INSIDE the major-tick inner end
    // (214) and overlapping the 30/60/90/... labels. Bumping the image
    // height to 230 makes the top edge land at radius 229, right
    // against the outer rim, so the triangle now lives entirely in
    // the outermost ring of the dial. Triangle depth stays 12 px.
    constexpr int kTwaW    = 28;
    constexpr int kTwaH    = 230;
    constexpr int kTwaTriH = 12;
    const size_t twa_buf_sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(kTwaW, kTwaH);
    lv_color_t* twa_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(twa_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (twa_buf) {
        lv_obj_t* twa_canvas = lv_canvas_create(compass);
        lv_canvas_set_buffer(twa_canvas, twa_buf, kTwaW, kTwaH,
                             LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(twa_canvas, lv_color_black(), LV_OPA_TRANSP);

        lv_draw_rect_dsc_t tri_dsc;
        lv_draw_rect_dsc_init(&tri_dsc);
        tri_dsc.bg_color = lv_palette_main(LV_PALETTE_AMBER);
        tri_dsc.bg_opa   = LV_OPA_COVER;
        const lv_point_t tri[3] = {
            {0,            0},      // base, rim-side, left
            {kTwaW - 1,    0},      // base, rim-side, right
            {kTwaW / 2,    kTwaTriH}, // apex pointing inward
        };
        lv_canvas_draw_polygon(twa_canvas, tri, 3, &tri_dsc);

        lv_obj_add_flag(twa_canvas, LV_OBJ_FLAG_HIDDEN);

        // Pivot at bottom-centre lands on the meter centre. Image's
        // top edge (where the triangle base sits) ends up at the rim.
        overview.twa_needle = lv_meter_add_needle_img(
            compass, scale,
            lv_canvas_get_img(twa_canvas),
            kTwaW / 2, kTwaH - 1);
        lv_meter_set_indicator_value(compass, overview.twa_needle, 0);
    } else {
        log_e("[ui] TWA canvas alloc failed (%u bytes) — true wind "
              "indicator disabled this boot",
              static_cast<unsigned>(twa_buf_sz));
        overview.twa_needle = nullptr;
    }

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
    lv_obj_set_style_border_width(inner, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(inner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    // Round 45: make the inner disc TRANSPARENT (was opaque black). With
    // the cone needle reaching almost to the centre and the user wanting
    // it visible "close to the centre circle", the opaque disc was
    // hiding the inner half of the cone. Transparent inner_disc keeps
    // its role as a positioning anchor for bow / DRIFT / AWS / heading
    // children (they still render on top of indicators because they're
    // children of the meter), while the cone now shows through to the
    // centre.
    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, LV_PART_MAIN);

    // ----- (5) Boat-outline silhouette (round 41 → removed round 42 → back round 43) -----
    //
    // Round 42 dropped the hull because the new reference image didn't
    // appear to show one. User pushed back ("no boat shape") so it's
    // back — but rendered as a FAINT white outline (LV_OPA_40) so it
    // reads as a subtle "this is a boat instrument" backdrop without
    // competing with the DRIFT pill / AWS box / wind pointer for
    // attention.
    //
    // Round 51: the round-50 parametric teardrop was too round-and-blob
    // shaped — user said "boat shape is horrible" and pointed at a
    // proper top-down hull picture (sharp pointed bow, sides curving
    // out then running nearly parallel through the midsection, flat
    // transom at the back). Replaced with a hand-tuned 24-point
    // polygon that tracks that reference shape.
    //
    // All coords are in inner_disc local space (320×320, centred at
    // (160, 160)). The polygon goes CW starting at the bow tip:
    //   * bow tip at the top of inner_disc (y=20)
    //   * sides curve outward through y=80 (bow shoulder) down to
    //     y=175 where they reach the maximum beam (270, 175) /
    //     (50, 175) — 220 px wide
    //   * sides taper gently from y=175 down to the transom shoulders
    //     near (235, 285) / (85, 285)
    //   * flat-ish transom across the bottom: (175, 300) → (145, 300)
    // Round 52 (per user "boat shape is too wide in the middle — it
    // should be a slim boat"): half-beam 110 → 50 (beam 100 px) for an
    // overall length:beam ratio of ≈ 2.8:1, much closer to the user's
    // reference hull picture. Length stays 280 px (bow at y=20, transom
    // at y=300). Re-spaced the 24 polygon points so the sides curve
    // gently outward, run nearly parallel through the long midship,
    // then taper to a 20 px wide flat transom.
    static const lv_point_t hull_pts[] = {
        {160,  20},   // bow tip
        {168,  38},
        {178,  60},
        {188,  90},
        {198, 120},
        {206, 150},
        {210, 175},   // beam right (widest, half-beam 50)
        {210, 205},
        {205, 235},
        {195, 260},
        {180, 285},
        {170, 296},
        {165, 300},   // right transom corner
        {155, 300},   // left transom corner
        {150, 296},
        {140, 285},
        {125, 260},
        {115, 235},
        {110, 205},
        {110, 175},   // beam left (widest, mirror)
        {114, 150},
        {122, 120},
        {132,  90},
        {142,  60},
        {152,  38},
        {160,  20},   // close back to bow tip
    };
    lv_obj_t* hull = lv_line_create(inner);
    lv_line_set_points(hull, hull_pts,
                       sizeof(hull_pts) / sizeof(hull_pts[0]));
    lv_obj_set_style_line_color(hull, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_line_width(hull, 4, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(hull, true, LV_PART_MAIN);
    lv_obj_set_style_line_opa(hull, LV_OPA_COVER, LV_PART_MAIN);

    // ----- (6) Centre DRIFT circle + AWS box below -----
    //
    // Round 44 layout per user "Middle should be a circle with a small
    // arrow and center should have drift (small angle) ... Below center
    // circle we have AWS":
    //   - DRIFT circle: small white-bordered circle at the geometric
    //     centre of the inner disc, holds the drift value (small font)
    //     and a tiny cyan arrow indicating direction.
    //   - AWS box: rectangular black-on-white-border box below the
    //     centre circle, holds apparent wind speed at large font.
    //
    // We don't have a real drift PGN on the bus, so the value is STW
    // (closest analogue — speed through water) and the arrow is pinned
    // pointing up for v1.

    // (6a) Centre DRIFT circle.
    {
        constexpr int kDriftSize = 78;
        lv_obj_t* circ = lv_obj_create(inner);
        lv_obj_set_size(circ, kDriftSize, kDriftSize);
        lv_obj_align(circ, LV_ALIGN_CENTER, 0, -10);
        lv_obj_set_style_radius(circ, kDriftSize / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(circ, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_width(circ, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(circ, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(circ, 0, LV_PART_MAIN);
        lv_obj_clear_flag(circ, LV_OBJ_FLAG_SCROLLABLE);

        overview.drift_value_lbl = makeLabel(circ, &lv_font_montserrat_20,
                                             lv_color_white(), "--");
        lv_obj_align(overview.drift_value_lbl, LV_ALIGN_CENTER, -6, -8);

        lv_obj_t* unit = makeLabel(circ, &lv_font_montserrat_12,
                                   lv_color_white(), "k");
        lv_obj_align(unit, LV_ALIGN_CENTER, 14, -10);

        lv_obj_t* sub = makeLabel(circ, &lv_font_montserrat_12,
                                  lv_palette_lighten(LV_PALETTE_GREY, 2),
                                  "DRIFT");
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 18);

        // Small cyan arrow inside the circle (pinned upward — drift
        // direction would rotate this in a future round once we have
        // the data).
        static const lv_point_t small_arrow[] = {
            { 5, 0},
            {10, 8},
            { 0, 8},
            { 5, 0},
        };
        lv_obj_t* arrow = lv_line_create(circ);
        lv_line_set_points(arrow, small_arrow,
                           sizeof(small_arrow) / sizeof(small_arrow[0]));
        lv_obj_set_style_line_color(arrow,
                                    lv_palette_main(LV_PALETTE_CYAN),
                                    LV_PART_MAIN);
        lv_obj_set_style_line_width(arrow, 2, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(arrow, true, LV_PART_MAIN);
        lv_obj_align(arrow, LV_ALIGN_TOP_MID, 0, 4);
    }

    // (6b) Round 52 — AWS readout moved OUTSIDE the boat (left side)
    // and a new BSPD readout added on the OTHER side (right). Both are
    // bare labels (value at montserrat_28, "k" unit at montserrat_14,
    // subtitle at montserrat_12) sitting on the dial annulus between
    // the slim hull and the dial labels.
    //
    // AWS — left of the hull.
    {
        overview.aws_value_lbl = makeLabel(inner, &lv_font_montserrat_28,
                                           lv_color_white(), "--");
        lv_obj_align(overview.aws_value_lbl, LV_ALIGN_CENTER, -100, -6);

        lv_obj_t* unit = makeLabel(inner, &lv_font_montserrat_14,
                                   lv_color_white(), "k");
        lv_obj_align(unit, LV_ALIGN_CENTER, -75, -14);

        lv_obj_t* sub = makeLabel(inner, &lv_font_montserrat_12,
                                  lv_palette_lighten(LV_PALETTE_GREY, 2),
                                  "AWS");
        lv_obj_align(sub, LV_ALIGN_CENTER, -100, 16);
    }

    // BSPD (boat speed through water) — right of the hull.
    {
        overview.bspd_value_lbl = makeLabel(inner, &lv_font_montserrat_28,
                                            lv_color_white(), "--");
        lv_obj_align(overview.bspd_value_lbl, LV_ALIGN_CENTER, 100, -6);

        lv_obj_t* unit = makeLabel(inner, &lv_font_montserrat_14,
                                   lv_color_white(), "k");
        lv_obj_align(unit, LV_ALIGN_CENTER, 125, -14);

        lv_obj_t* sub = makeLabel(inner, &lv_font_montserrat_12,
                                  lv_palette_lighten(LV_PALETTE_GREY, 2),
                                  "BSPD");
        lv_obj_align(sub, LV_ALIGN_CENTER, 100, 16);
    }

    // ----- (7) Heading marker — small white triangle at bottom of disc -----
    {
        static const lv_point_t tri_pts[] = {
            {10,  0},
            {20, 18},
            { 0, 18},
            {10,  0},
        };
        lv_obj_t* tri = lv_line_create(inner);
        lv_line_set_points(tri, tri_pts,
                           sizeof(tri_pts) / sizeof(tri_pts[0]));
        lv_obj_set_style_line_color(tri, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_line_width(tri, 2, LV_PART_MAIN);
        lv_obj_set_style_line_rounded(tri, true, LV_PART_MAIN);
        lv_obj_align(tri, LV_ALIGN_BOTTOM_MID, 0, -14);
    }
}

// Round 54 — buildMainPage / buildSimulatorPage replace the
// round-41-52 sparkline data grid. The data grid (DataCell, DataPage,
// MetricHistory, CellSpec, kSpecs, buildDataCell, buildDataPage,
// recordHistorySnapshot, refreshDataCell, refreshData, metricValue,
// metricFmt, CellMetric) was removed wholesale.

// Build the Main display page — central boat with a fixed outer 0..180
// mirrored ring and a rotating compass ring inside it.
void buildMainPage() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleScreen(scr);
    main_pg.root = scr;

    // ----- Outer ring (FIXED) -----
    constexpr int kCompassSize = 460;
    lv_obj_t* compass = lv_meter_create(scr);
    lv_obj_set_size(compass, kCompassSize, kCompassSize);
    lv_obj_align(compass, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(compass, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(compass, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(compass, 0, LV_PART_MAIN);
    lv_obj_clear_flag(compass, LV_OBJ_FLAG_SCROLLABLE);
    // Hide auto labels (we use manual mirrored labels instead).
    lv_obj_set_style_text_color(compass, lv_color_black(), LV_PART_TICKS);
    lv_obj_set_style_text_font(compass, &lv_font_montserrat_20, LV_PART_TICKS);

    lv_meter_scale_t* scale = lv_meter_add_scale(compass);
    lv_meter_set_scale_ticks(compass, scale, 145, 1, 8, lv_color_white());
    lv_meter_set_scale_major_ticks(compass, scale, 12, 3, 16,
                                   lv_color_white(), 18);
    lv_meter_set_scale_range(compass, scale, 0, 360, 360, 270);
    main_pg.compass = compass;

    // Manual mirrored labels (0..180 each side of the bow).
    {
        constexpr float kLabelRadius = 188.0f;
        for (int i = 0; i < 12; ++i) {
            const float angle_deg = static_cast<float>(i) * 30.0f;
            const float a = angle_deg * static_cast<float>(M_PI) / 180.0f;
            const int dx = static_cast<int>(kLabelRadius * sinf(a));
            const int dy = -static_cast<int>(kLabelRadius * cosf(a));
            const int val = (i <= 6) ? (i * 30) : ((12 - i) * 30);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", val);
            lv_obj_t* lbl = makeLabel(compass, &lv_font_montserrat_20,
                                      lv_color_white(), buf);
            lv_obj_align(lbl, LV_ALIGN_CENTER, dx, dy);
        }
    }

    // Close-hauled red/green sectors. Round 55: 20°-60° each side of the
    // bow (was 0°-60°). Leaves a clean 40° "bow zone" with no colour
    // around dead-ahead, matching the user's reference where the no-go
    // markers don't touch the bow.
    main_pg.port_sector = lv_meter_add_arc(
        compass, scale, 26, lv_color_hex(0xCC0000), -22);
    lv_meter_set_indicator_start_value(compass, main_pg.port_sector, 300);
    lv_meter_set_indicator_end_value  (compass, main_pg.port_sector, 340);

    main_pg.stbd_sector = lv_meter_add_arc(
        compass, scale, 26, lv_color_hex(0x006400), -22);
    lv_meter_set_indicator_start_value(compass, main_pg.stbd_sector, 20);
    lv_meter_set_indicator_end_value  (compass, main_pg.stbd_sector, 60);

    // ----- Blue "T" target marker on the outer rim -----
    //
    // Built as a 36×230 lv_canvas with a blue triangle in the top 26 px
    // and the letter "T" stamped on top, registered as a needle_img so
    // we can rotate it to the set-course angle. Bottom 204 px are
    // transparent (just for the rotation pivot).
    constexpr int kTgtW = 36;
    constexpr int kTgtH = 230;
    constexpr int kTgtTriH = 26;
    const size_t tgt_buf_sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(kTgtW, kTgtH);
    lv_color_t* tgt_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(tgt_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (tgt_buf) {
        lv_obj_t* tgt_canvas = lv_canvas_create(compass);
        lv_canvas_set_buffer(tgt_canvas, tgt_buf, kTgtW, kTgtH,
                             LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(tgt_canvas, lv_color_black(), LV_OPA_TRANSP);

        lv_draw_rect_dsc_t tri_dsc;
        lv_draw_rect_dsc_init(&tri_dsc);
        tri_dsc.bg_color = lv_palette_main(LV_PALETTE_BLUE);
        tri_dsc.bg_opa   = LV_OPA_COVER;
        const lv_point_t tri[3] = {
            {0,            0},
            {kTgtW - 1,    0},
            {kTgtW / 2,    kTgtTriH},
        };
        lv_canvas_draw_polygon(tgt_canvas, tri, 3, &tri_dsc);
        // Round 55: dropped the "T" letter — user described it as just a
        // "blue triangle" in the round-55 spec, so the marker is now a
        // clean coloured triangle on the rim.
        lv_obj_add_flag(tgt_canvas, LV_OBJ_FLAG_HIDDEN);

        main_pg.target_marker = lv_meter_add_needle_img(
            compass, scale,
            lv_canvas_get_img(tgt_canvas),
            kTgtW / 2, kTgtH - 1);
        lv_meter_set_indicator_value(compass, main_pg.target_marker, 0);
    } else {
        main_pg.target_marker = nullptr;
    }

    // ----- TWD wide-arrow cone -----
    //
    // Reuses the round-46 cone canvas pattern but rotated to TWD instead
    // of AWA. Cone tip at the rim, base just above the boat hull.
    constexpr int kConeW           = 50;
    constexpr int kConeH           = 140;
    constexpr int kConeVisibleBase = 90;
    const size_t cone_buf_sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(kConeW, kConeH);
    lv_color_t* cone_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(cone_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (cone_buf) {
        lv_obj_t* cone_canvas = lv_canvas_create(compass);
        lv_canvas_set_buffer(cone_canvas, cone_buf, kConeW, kConeH,
                             LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(cone_canvas, lv_color_black(), LV_OPA_TRANSP);
        lv_draw_rect_dsc_t outer_dsc;
        lv_draw_rect_dsc_init(&outer_dsc);
        outer_dsc.bg_color = lv_color_white();
        outer_dsc.bg_opa   = LV_OPA_COVER;
        const lv_point_t outer_tri[3] = {
            {kConeW / 2,                  0},
            {kConeW - 5, kConeVisibleBase   },
            {         5, kConeVisibleBase   },
        };
        lv_canvas_draw_polygon(cone_canvas, outer_tri, 3, &outer_dsc);
        lv_draw_rect_dsc_t inner_dsc;
        lv_draw_rect_dsc_init(&inner_dsc);
        inner_dsc.bg_color = lv_palette_main(LV_PALETTE_BLUE);
        inner_dsc.bg_opa   = LV_OPA_COVER;
        const lv_point_t inner_tri[3] = {
            {kConeW / 2,                   6},
            {kConeW - 8, kConeVisibleBase - 4},
            {         8, kConeVisibleBase - 4},
        };
        lv_canvas_draw_polygon(cone_canvas, inner_tri, 3, &inner_dsc);
        lv_obj_add_flag(cone_canvas, LV_OBJ_FLAG_HIDDEN);

        main_pg.twd_arrow = lv_meter_add_needle_img(
            compass, scale,
            lv_canvas_get_img(cone_canvas),
            kConeW / 2, kConeH - 1);
        lv_meter_set_indicator_value(compass, main_pg.twd_arrow, 180); // pointing down by default
    } else {
        main_pg.twd_arrow = nullptr;
    }

    // ----- Rotating inner compass ring (children of compass widget) -----
    //
    // Container holds 12 numeric labels positioned on a circle. Setting
    // transform_pivot to the container centre and transform_angle to
    // -heading rotates the whole ring together so the boat's actual
    // heading appears at the top of the ring.
    constexpr int kInnerRingSize = 360;
    main_pg.inner_ring = lv_obj_create(compass);
    lv_obj_set_size(main_pg.inner_ring, kInnerRingSize, kInnerRingSize);
    lv_obj_align(main_pg.inner_ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(main_pg.inner_ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(main_pg.inner_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(main_pg.inner_ring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(main_pg.inner_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(main_pg.inner_ring,
                                       kInnerRingSize / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(main_pg.inner_ring,
                                       kInnerRingSize / 2, LV_PART_MAIN);
    {
        constexpr float kInnerLabelR = 150.0f;
        for (int i = 0; i < 12; ++i) {
            const float angle_deg = static_cast<float>(i) * 30.0f;
            const float a = angle_deg * static_cast<float>(M_PI) / 180.0f;
            const int dx = static_cast<int>(kInnerLabelR * sinf(a));
            const int dy = -static_cast<int>(kInnerLabelR * cosf(a));
            const int val = i * 30;  // 000, 030, 060 ... 330
            char buf[8];
            snprintf(buf, sizeof(buf), "%03d", val);
            lv_obj_t* lbl = makeLabel(main_pg.inner_ring,
                                      &lv_font_montserrat_16,
                                      i == 0 ? lv_palette_main(LV_PALETTE_GREEN)
                                             : lv_palette_lighten(LV_PALETTE_GREY, 2),
                                      i == 0 ? "N" : buf);
            lv_obj_align(lbl, LV_ALIGN_CENTER, dx, dy);
        }
    }

    // Round 55: second blue target triangle, on the ROTATING inner ring,
    // marking the desired compass course. Drawn into a small lv_canvas
    // and placed at angle = TARGET_COURSE inside the container (not
    // refreshed per frame — the container's transform_angle handles
    // following heading). Pre-rotated so its apex points toward the
    // container centre at the target bearing.
    //
    // For v1 the desired course is hardcoded at 60° true; a future
    // round will tie it to a real autopilot SET command on the bus.
    constexpr float kTargetCourse = 60.0f;
    constexpr int   kInnerTgtSize = 22;
    const size_t inner_tgt_buf_sz =
        LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(kInnerTgtSize, kInnerTgtSize);
    lv_color_t* inner_tgt_buf = static_cast<lv_color_t*>(
        heap_caps_malloc(inner_tgt_buf_sz,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (inner_tgt_buf) {
        lv_obj_t* inner_tgt = lv_canvas_create(main_pg.inner_ring);
        lv_canvas_set_buffer(inner_tgt, inner_tgt_buf,
                             kInnerTgtSize, kInnerTgtSize,
                             LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(inner_tgt, lv_color_black(), LV_OPA_TRANSP);
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_palette_main(LV_PALETTE_BLUE);
        dsc.bg_opa   = LV_OPA_COVER;
        const lv_point_t tri[3] = {
            {kInnerTgtSize / 2,            0},   // apex (canvas top)
            {kInnerTgtSize - 1, kInnerTgtSize - 1},
            {0,                  kInnerTgtSize - 1},
        };
        lv_canvas_draw_polygon(inner_tgt, tri, 3, &dsc);

        // Position the canvas at the target bearing on a circle of
        // radius kTgtR inside the container.
        constexpr float kTgtR = 130.0f;
        const float a = kTargetCourse * static_cast<float>(M_PI) / 180.0f;
        const int cx = 180 - kInnerTgtSize / 2 + static_cast<int>(kTgtR * sinf(a));
        const int cy = 180 - kInnerTgtSize / 2 - static_cast<int>(kTgtR * cosf(a));
        lv_obj_set_pos(inner_tgt, cx, cy);

        // Pre-rotate so the apex points toward the container centre
        // when the container is at heading=0. (kTargetCourse + 180°)
        // because the canvas's local "up" is 0° and we want it to
        // point at the centre, which is in the opposite direction
        // from the canvas's position.
        lv_obj_set_style_transform_pivot_x(inner_tgt,
                                           kInnerTgtSize / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(inner_tgt,
                                           kInnerTgtSize / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_angle(
            inner_tgt,
            ((static_cast<int>(kTargetCourse) + 180) % 360) * 10,
            LV_PART_MAIN);
    }

    // ----- Boat hull at centre (slim 24-point polygon) -----
    static const lv_point_t hull_pts[] = {
        {230,  90}, {238, 108}, {248, 130}, {258, 160}, {268, 190},
        {276, 220}, {280, 245}, {280, 275}, {275, 305}, {265, 330},
        {250, 355}, {240, 366}, {235, 370}, {225, 370}, {220, 366},
        {210, 355}, {195, 330}, {185, 305}, {180, 275}, {180, 245},
        {184, 220}, {192, 190}, {202, 160}, {212, 130}, {222, 108},
        {230,  90},
    };
    lv_obj_t* hull = lv_line_create(compass);
    lv_line_set_points(hull, hull_pts,
                       sizeof(hull_pts) / sizeof(hull_pts[0]));
    lv_obj_set_style_line_color(hull, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_line_width(hull, 4, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(hull, true, LV_PART_MAIN);

    // ----- Heading label "038" at the bow -----
    {
        lv_obj_t* hdg_box = lv_obj_create(compass);
        lv_obj_set_size(hdg_box, 64, 30);
        lv_obj_align(hdg_box, LV_ALIGN_CENTER, 0, -130);
        lv_obj_set_style_radius(hdg_box, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(hdg_box, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(hdg_box, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(hdg_box, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(hdg_box, 0, LV_PART_MAIN);
        lv_obj_clear_flag(hdg_box, LV_OBJ_FLAG_SCROLLABLE);

        main_pg.heading_lbl = makeLabel(hdg_box, &lv_font_montserrat_20,
                                        lv_color_black(), "---");
        lv_obj_align(main_pg.heading_lbl, LV_ALIGN_CENTER, 0, 0);
    }

    // ----- Boat speed at the stern -----
    main_pg.bspd_lbl = makeLabel(compass, &lv_font_montserrat_28,
                                 lv_color_white(), "--");
    lv_obj_align(main_pg.bspd_lbl, LV_ALIGN_CENTER, 0, 145);

    // ----- Depth on the right side -----
    {
        lv_obj_t* depth_sub = makeLabel(compass, &lv_font_montserrat_12,
                                        lv_palette_lighten(LV_PALETTE_GREY, 2),
                                        "DEPTH");
        lv_obj_align(depth_sub, LV_ALIGN_CENTER, 110, -20);
        main_pg.depth_lbl = makeLabel(compass, &lv_font_montserrat_24,
                                      lv_color_white(), "--");
        lv_obj_align(main_pg.depth_lbl, LV_ALIGN_CENTER, 110, 0);
        lv_obj_t* depth_unit = makeLabel(compass, &lv_font_montserrat_12,
                                         lv_palette_lighten(LV_PALETTE_GREY, 2),
                                         "m");
        lv_obj_align(depth_unit, LV_ALIGN_CENTER, 110, 18);
    }

    // ----- Heel-angle indicator on the left side -----
    //
    // We don't have a heel sensor yet — the round-54 placeholder is just
    // a small "HEEL  0°" readout. A future round will add an actual
    // graphical tilt indicator (a vertical line that leans with the
    // boat) once a heel sensor / IMU is available on the bus.
    {
        lv_obj_t* heel_sub = makeLabel(compass, &lv_font_montserrat_12,
                                       lv_palette_lighten(LV_PALETTE_GREY, 2),
                                       "HEEL");
        lv_obj_align(heel_sub, LV_ALIGN_CENTER, -110, -20);
        main_pg.heel_lbl = makeLabel(compass, &lv_font_montserrat_24,
                                     lv_color_white(), "0\xC2\xB0");
        lv_obj_align(main_pg.heel_lbl, LV_ALIGN_CENTER, -110, 0);
    }
}

void buildSimulatorPage() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    styleScreen(scr);
    sim_pg.root = scr;

    // Header.
    lv_obj_t* header = makeLabel(scr, &lv_font_montserrat_16,
                                 lv_color_white(), "SIMULATOR — raw + derived");
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 24);

    // Two columns of label rows. Left column = raw sensor inputs, right
    // column = values BoatState computed from them. Each row is one
    // pair (label + value) drawn at a fixed y-offset.
    auto row = [&](int y, int x_label, int x_value, const char* name,
                   const lv_font_t* font = &lv_font_montserrat_14)
        -> lv_obj_t* {
        lv_obj_t* l = makeLabel(scr, font,
                                lv_palette_lighten(LV_PALETTE_GREY, 2),
                                name);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, x_label, y);
        lv_obj_t* v = makeLabel(scr, font, lv_color_white(), "--");
        lv_obj_align(v, LV_ALIGN_TOP_LEFT, x_value, y);
        return v;
    };

    constexpr int kColLeft   = 30;
    constexpr int kValLeft   = 110;
    constexpr int kColRight  = 250;
    constexpr int kValRight  = 330;
    constexpr int kRowH      = 24;
    int y = 60;

    // RAW (left column)
    sim_pg.lat_lbl   = row(y, kColLeft, kValLeft, "lat");      y += kRowH;
    sim_pg.lon_lbl   = row(y, kColLeft, kValLeft, "lon");      y += kRowH;
    sim_pg.sog_lbl   = row(y, kColLeft, kValLeft, "SOG");      y += kRowH;
    sim_pg.cog_lbl   = row(y, kColLeft, kValLeft, "COG");      y += kRowH;
    sim_pg.awa_lbl   = row(y, kColLeft, kValLeft, "AWA");      y += kRowH;
    sim_pg.aws_lbl   = row(y, kColLeft, kValLeft, "AWS");      y += kRowH;
    sim_pg.hdg_m_lbl = row(y, kColLeft, kValLeft, "HDG\xC2\xB0M"); y += kRowH;
    sim_pg.var_lbl   = row(y, kColLeft, kValLeft, "VAR");      y += kRowH;
    sim_pg.stw_lbl   = row(y, kColLeft, kValLeft, "STW");      y += kRowH;
    sim_pg.depth_lbl = row(y, kColLeft, kValLeft, "depth");

    // DERIVED (right column)
    int yr = 60;
    sim_pg.hdg_t_lbl = row(yr, kColRight, kValRight, "HDG\xC2\xB0T"); yr += kRowH;
    sim_pg.twa_lbl   = row(yr, kColRight, kValRight, "TWA");          yr += kRowH;
    sim_pg.tws_lbl   = row(yr, kColRight, kValRight, "TWS");          yr += kRowH;
    sim_pg.twd_lbl   = row(yr, kColRight, kValRight, "TWD");          yr += kRowH;
    sim_pg.vmg_lbl   = row(yr, kColRight, kValRight, "VMG");
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

// Round 53: TWD and VMG were previously computed inline here. Now they
// live in BoatState::recomputeDerived_locked() (alongside heading_true,
// TWA, TWS) and are exposed as plain fields on the Instruments snapshot.
// The metricValue switch below just reads s.twd / s.vmg directly.

void refreshOverview(const Instruments& s) {
    // Round 42 (flicker mitigation, bounce-buffer fallback path): dedup
    // wind-pointer redraws. lv_meter_set_indicator_value unconditionally
    // marks the entire meter widget dirty, which on our 460×460 dial is
    // a ~423 KB flush — too big for vblank, so the memcpy races the RGB
    // scan and we get the side-to-side shimmer. Skipping the call when
    // the integer angle hasn't changed cuts redraws by 5-10× in steady
    // state (the simulator and the real bus both deliver sub-degree
    // jitter at 10 Hz, but the visible angle rarely steps more than
    // 1-2°/s).
    static int32_t last_awa = INT32_MIN;
    if (!isnan(s.awa) && overview.awa_needle != nullptr) {
        double deg = s.awa < 0 ? s.awa + 360.0 : s.awa;
        const int32_t v = static_cast<int32_t>(deg);
        if (v != last_awa) {
            lv_meter_set_indicator_value(overview.compass,
                                         overview.awa_needle, v);
            last_awa = v;
        }
    }

    // Round 45: TWA (true wind angle) yellow needle, same dedup pattern
    // as AWA. TWA same convention as AWA: -180..180 with + starboard.
    static int32_t last_twa = INT32_MIN;
    if (!isnan(s.twa) && overview.twa_needle != nullptr) {
        double deg = s.twa < 0 ? s.twa + 360.0 : s.twa;
        const int32_t v = static_cast<int32_t>(deg);
        if (v != last_twa) {
            lv_meter_set_indicator_value(overview.compass,
                                         overview.twa_needle, v);
            last_twa = v;
        }
    }

    // DRIFT pill — speed-through-water (paddlewheel). The reference's
    // "DRIFT" label technically refers to current/leeway, which we don't
    // get on the bus; STW (with SOG fallback) is the closest analogue
    // and matches what the user wants to see at a glance.
    const double drift = !isnan(s.stw) ? s.stw : s.sog;
    if (isnan(drift)) {
        lv_label_set_text(overview.drift_value_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", drift);
        lv_label_set_text(overview.drift_value_lbl, buf);
    }

    // AWS box — apparent wind speed in knots, one decimal.
    if (isnan(s.aws)) {
        lv_label_set_text(overview.aws_value_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", s.aws);
        lv_label_set_text(overview.aws_value_lbl, buf);
    }

    // BSPD readout (round 52, right of the hull) — boat speed through
    // water, falling back to SOG when STW isn't on the bus yet.
    const double bspd = !isnan(s.stw) ? s.stw : s.sog;
    if (isnan(bspd)) {
        lv_label_set_text(overview.bspd_value_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", bspd);
        lv_label_set_text(overview.bspd_value_lbl, buf);
    }
}

// Round 54: refreshMain — Main display page. Updates the rotating
// inner compass ring (transform_angle = -heading), the heading label
// at the bow, the BSPD readout at the stern, the TWD wide-arrow needle,
// the depth readout on the right, and a placeholder heel readout on
// the left. The blue T target marker on the rim is pinned to
// heading + 30° for v1 (will follow a real autopilot SET command in a
// future round once we expose that on the bus).
void refreshMain(const Instruments& s) {
    // Rotating inner compass ring. transform_angle is in 0.1° units;
    // negative because rotating the LABELS opposite to the heading
    // change keeps the boat's actual heading at the top of the ring.
    if (!isnan(s.heading_true_deg) && main_pg.inner_ring) {
        const int16_t ang_01 =
            -static_cast<int16_t>(std::lround(s.heading_true_deg * 10.0));
        lv_obj_set_style_transform_angle(main_pg.inner_ring,
                                         ang_01, LV_PART_MAIN);
    }

    // Heading label at the bow.
    if (isnan(s.heading_true_deg)) {
        lv_label_set_text(main_pg.heading_lbl, "---");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%03.0f", s.heading_true_deg);
        lv_label_set_text(main_pg.heading_lbl, buf);
    }

    // BSPD at the stern.
    const double bspd = !isnan(s.stw) ? s.stw : s.sog;
    if (isnan(bspd)) {
        lv_label_set_text(main_pg.bspd_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", bspd);
        lv_label_set_text(main_pg.bspd_lbl, buf);
    }

    // Depth on the right side.
    if (isnan(s.depth_m)) {
        lv_label_set_text(main_pg.depth_lbl, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", s.depth_m);
        lv_label_set_text(main_pg.depth_lbl, buf);
    }

    // TWD wide-arrow rotated to true wind direction in BOAT-relative
    // angle (the compass widget's scale is 0..360 with 0 = bow). TWD
    // is in degrees true; convert to boat frame: twd - heading_true,
    // normalised to 0..360.
    if (main_pg.twd_arrow != nullptr &&
        !isnan(s.twd) && !isnan(s.heading_true_deg)) {
        double rel = s.twd - s.heading_true_deg;
        while (rel <    0.0) rel += 360.0;
        while (rel >= 360.0) rel -= 360.0;
        lv_meter_set_indicator_value(main_pg.compass,
                                     main_pg.twd_arrow,
                                     static_cast<int32_t>(rel));
    }

    // Round 55: outer-rim target triangle at the BOAT-RELATIVE bearing
    // to the desired course. The hardcoded target (60° true, see
    // buildMainPage's kTargetCourse) is in compass coords; convert to
    // bow-relative by subtracting current heading and normalising into
    // the meter's 0..360 scale. Hidden if heading isn't known yet.
    if (main_pg.target_marker != nullptr) {
        constexpr double kTargetCourse = 60.0;
        if (!isnan(s.heading_true_deg)) {
            double rel = kTargetCourse - s.heading_true_deg;
            while (rel <    0.0) rel += 360.0;
            while (rel >= 360.0) rel -= 360.0;
            lv_meter_set_indicator_value(main_pg.compass,
                                         main_pg.target_marker,
                                         static_cast<int32_t>(rel));
        }
    }
}

// Round 54: refreshSimulator — populate the labels on Page 3 with the
// raw sensor values being fed into BoatState plus the values BoatState
// computed from them.
void refreshSimulator(const Instruments& s) {
    char buf[24];
    auto setVal = [&](lv_obj_t* lbl, double v, const char* fmt) {
        if (lbl == nullptr) return;
        if (isnan(v)) {
            lv_label_set_text(lbl, "--");
        } else {
            snprintf(buf, sizeof(buf), fmt, v);
            lv_label_set_text(lbl, buf);
        }
    };

    // Raw.
    setVal(sim_pg.lat_lbl,   s.lat,                    "%.4f");
    setVal(sim_pg.lon_lbl,   s.lon,                    "%.4f");
    setVal(sim_pg.sog_lbl,   s.sog,                    "%.1f kn");
    setVal(sim_pg.cog_lbl,   s.cog,                    "%03.0f\xC2\xB0");
    setVal(sim_pg.awa_lbl,   s.awa,                    "%+.0f\xC2\xB0");
    setVal(sim_pg.aws_lbl,   s.aws,                    "%.1f kn");
    setVal(sim_pg.hdg_m_lbl, s.heading_mag_deg,        "%03.0f\xC2\xB0");
    setVal(sim_pg.var_lbl,   s.magnetic_variation_deg, "%+.1f\xC2\xB0");
    setVal(sim_pg.stw_lbl,   s.stw,                    "%.1f kn");
    setVal(sim_pg.depth_lbl, s.depth_m,                "%.1f m");

    // Derived.
    setVal(sim_pg.hdg_t_lbl, s.heading_true_deg, "%03.0f\xC2\xB0");
    setVal(sim_pg.twa_lbl,   s.twa,              "%+.0f\xC2\xB0");
    setVal(sim_pg.tws_lbl,   s.tws,              "%.1f kn");
    setVal(sim_pg.twd_lbl,   s.twd,              "%03.0f\xC2\xB0");
    setVal(sim_pg.vmg_lbl,   s.vmg,              "%.1f kn");
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
    const Instruments s = g_state->snapshot();

    // Only refresh the currently visible page — cheaper, and hidden pages
    // will refresh next time they're shown.
    switch (current_page) {
        case 0: refreshMain(s);            break;
        case 1: refreshOverview(s);        break;
        case 2: refreshDebug();            break;
        case 3: refreshSimulator(s);       break;
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
// Round 57: dropout-debounced touch read. The CST820 reports "no
// finger" intermittently mid-touch — a known LVGL-forum gotcha for
// CST816/CST820 — so the round-55 state machine kept seeing false
// release events during a swipe, which reset press_x and tanked the
// dx delta. Now we ride out brief no-finger gaps for up to
// kHoldThroughGapMs and only declare a real release when contact has
// been absent for that long. press_x is captured on the FIRST rising
// edge after a real release; intra-gap dropouts don't disturb it.
void touchReadCb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
    static int16_t  last_x              = 0;
    static int16_t  last_y              = 0;
    static int16_t  press_x             = 0;
    static int16_t  press_y             = 0;
    static uint32_t press_ms            = 0;
    static uint32_t last_real_press_ms  = 0;
    static bool     reported_pressed    = false;
    // Round 63: latches true the first time the chip's onboard gesture
    // engine reports a horizontal-slide code during the current touch
    // sequence. Used to (a) suppress duplicate firing if the chip
    // re-reports the same code on a later tick within the same touch
    // and (b) skip the dx/dy fallback at release time so a single
    // swipe doesn't queue two page steps.
    static bool     gesture_fired_this_touch = false;

    // Round 61: hold-through gap 250 → 1200 ms. The round-60 trace
    // showed every dx=0 failure had held≈455 ms exactly — i.e. ~205 ms
    // of chip activity + 250 ms debounce. That 205 ms is the CST820's
    // tap-detection window: when the user presses without immediate
    // motion the chip classifies the touch as a tap candidate and
    // stops streaming coordinates while it waits to see if anything
    // happens. If the user starts moving AFTER that window, our 250 ms
    // gap timed out before the chip resumed streaming, so we declared
    // a false release. 1200 ms gives a generous ride-through window
    // — enough to cover a press-pause-move pattern with slack to
    // spare. A real release without further input still cleanly
    // declares as released after 1.2 s, fine for tap responses we'll
    // wire up later.
    constexpr uint32_t kHoldThroughGapMs = 1200;

    uint16_t raw_x = 0, raw_y = 0;
    uint8_t  gesture = 0;
    bool     lift_event = false;
    const bool fresh = g_touch.read(&raw_x, &raw_y, &gesture, &lift_event);
    const uint32_t now = millis();

    if (fresh) {
        last_x = static_cast<int16_t>(DISPLAY_WIDTH  - 1 - raw_x);
        last_y = static_cast<int16_t>(DISPLAY_HEIGHT - 1 - raw_y);
        last_real_press_ms = now;
        if (!reported_pressed) {
            // Rising edge after a true release — anchor the swipe.
            press_x  = last_x;
            press_y  = last_y;
            press_ms = now;
            gesture_fired_this_touch = false;  // round 63
            // Round 58: log the swipe-anchor point so a serial trace
            // shows exactly where the gesture started.
            log_i("[ui] touch DOWN at (%d, %d)", last_x, last_y);
        }
    }

    // Round 64: gesture-fire check moved out of the `if (fresh)` block.
    // The chip occasionally latches the slide code on the no-finger tick
    // immediately after lift (round-63 bench showed three quick-swipe
    // attempts where the chip never streamed coords AND never fired the
    // gesture during the press window — but it might have fired post-lift,
    // and the round-63 driver was discarding gesture on no-finger reads).
    // gesture_fired_this_touch latches per touch sequence, so this is
    // still single-fire. reported_pressed gates against stale codes from
    // an already-evaluated touch.
    //
    // Frame mapping: the panel is mounted upside-down and we apply a
    // 180° software rotation in flushCb / coordinate read above, so
    // chip-frame "swipe right" (0x04) is what the user perceives as a
    // left-swipe → next page (+1). Mirror for 0x03.
    //
    // Up/down chip codes (0x01/0x02) are deliberately not wired — we
    // only navigate horizontally.
    if (reported_pressed && !gesture_fired_this_touch &&
        (gesture == 0x03 || gesture == 0x04)) {
        g_pending_page_step = (gesture == 0x03) ? -1 : +1;
        gesture_fired_this_touch = true;
        log_i("[ui] chip gesture 0x%02X → page step %+d (fresh=%d lift=%d)",
              gesture, (int)g_pending_page_step,
              fresh ? 1 : 0, lift_event ? 1 : 0);
    }

    // Round 64: lift_event is the chip's "finger just released" signal,
    // and the coords on this tick are the FINAL touch position. Declare
    // release immediately — no need to wait for the hold-through gap.
    bool effective;
    if (fresh && lift_event) {
        effective = false;  // lift = release this tick
    } else if (fresh) {
        effective = true;
    } else if (reported_pressed &&
               (now - last_real_press_ms) < kHoldThroughGapMs) {
        // CST820 dropped finger this sample but we were just touched —
        // hold the press state for the gap window (round 61).
        effective = true;
    } else {
        effective = false;
    }

    if (effective) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        // Either we were already released last call, or the gap window
        // just elapsed → this is a TRUE release. Evaluate the swipe.
        if (reported_pressed) {
            // Round 61: measure held_ms from the LAST real chip contact,
            // not from `now`. `now` is the moment kHoldThroughGapMs
            // elapsed and we declared release; the actual finger-down
            // span ended at last_real_press_ms. With the gap bumped
            // to 1200 ms, computing from `now` was eating 1.2 s into
            // every swipe's kSwipeMaxMs budget.
            const uint32_t held_ms = last_real_press_ms - press_ms;
            const int32_t dx = static_cast<int32_t>(last_x) - press_x;
            const int32_t dy = static_cast<int32_t>(last_y) - press_y;
            // Round 62: motion + horizontal-bias only; duration check
            // dropped (see kSwipeMaxMs comment for why).
            // Round 63: skip the dx/dy fallback entirely if the chip's
            // onboard gesture engine already fired during this touch —
            // otherwise a single swipe queues two page steps.
            (void)kSwipeMaxMs;
            const bool qualifies = !gesture_fired_this_touch &&
                                   (std::abs(dx) >= kSwipeMinPx &&
                                    std::abs(dx) >  std::abs(dy));
            // Round 70: tap fallback. Rounds 65-69 nailed down that the
            // CST820 on this board goes silent for ~entirety of isolated
            // touches (one coord at press, then nothing — no motion
            // samples, no gesture byte, no TP_INT pulses) and no register
            // tweak changes that. Polling sometimes catches a stream of
            // mid-touch coords; usually it doesn't. Result: rounds-65-69
            // hovered between 0% and 86% swipe detection, dependent on
            // user touch cadence in ways we can't influence.
            //
            // Tap fallback: when we get to release with no gesture fired
            // and no qualifying dx/dy swipe, use the PRESS location for
            // half-screen nav. Same direction mapping as swipes — left
            // half is "previous" (chip-cooperative swipe-right also
            // queues -1), right half is "next" (chip-cooperative
            // swipe-left also queues +1) — so the user-visible behaviour
            // stays consistent whether the chip cooperated or not.
            const bool tap_fallback = !gesture_fired_this_touch && !qualifies;
            if (qualifies) {
                g_pending_page_step = (dx > 0) ? -1 : +1;
            } else if (tap_fallback) {
                g_pending_page_step =
                    (press_x < DISPLAY_WIDTH / 2) ? -1 : +1;
            }
            // Round 58: log every release with the qualifier verdict so
            // we can see from a swipe attempt why it did or didn't fire.
            // Round 63: distinguish chip-gesture firings from dx/dy ones.
            // Round 64: tag the release source — [lift] means we got the
            // chip's lift-event coord this tick (good — final coord is
            // accurate), [gap] means hold-through expired with no lift
            // event (chip went silent through the entire touch).
            // Round 70: tag tap-fallback firings with the resolved half.
            log_i("[ui] touch UP at (%d, %d) dx=%ld dy=%ld held=%lums %s%s",
                  last_x, last_y, (long)dx, (long)dy,
                  (unsigned long)held_ms,
                  lift_event ? "[lift] " : "[gap] ",
                  gesture_fired_this_touch
                      ? "(chip gesture already fired)"
                      : (qualifies
                             ? "→ SWIPE"
                             : (press_x < DISPLAY_WIDTH / 2
                                    ? "→ TAP-LEFT (prev)"
                                    : "→ TAP-RIGHT (next)")));
            gesture_fired_this_touch = false;
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }

    data->point.x = last_x;
    data->point.y = last_y;
    reported_pressed = effective;
}

// --- Display driver glue ---------------------------------------------------

// LVGL -> ST7701 glue. LVGL hands us a partial-screen rectangle of RGB565
// pixels (LV_COLOR_DEPTH=16, LV_COLOR_16_SWAP=0 — see lv_conf.h round-49
// notes); we forward it to the panel.
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
    buildMainPage();
    buildOverviewPage();
    buildDebugPage();
    buildSimulatorPage();
    pages[0] = main_pg.root;       // Main display (Round 54: was wind page)
    pages[1] = overview.root;      // Wind display
    pages[2] = debug_pg.root;      // Debug page
    pages[3] = sim_pg.root;        // Simulator page (raw + derived values)

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
    const uint32_t now = millis();
    // Round 42: throttle dropped from 100 ms (10 Hz, round 35) to 250 ms
    // (4 Hz). Reasoning: the bounce-buffer flicker fix didn't make it
    // (IDF struct in this Arduino-ESP32 release lacks the field), so
    // every full refresh still risks a beam-race when the lv_meter
    // recomposites. Quartering the refresh rate quarters the flicker
    // opportunity without making the readout feel laggy — boat
    // instruments at 4 Hz are still well under any human reaction
    // threshold for "is the wind pointer moving?".
    if (now - last_data_refresh_ms >= 250) {
        last_data_refresh_ms = now;
        refreshFromState();
    }
    // Round 54: the per-metric 1 Hz history-sampling block was removed
    // along with the rest of the data-grid page (Page 2 sparklines). No
    // current page consumes a rolling history buffer.

    // Round 39: apply any pending page change queued by touchReadCb's
    // tap detection. We do this here (between LVGL timer ticks) instead
    // of in the indev callback because lv_scr_load() needs to dispatch
    // events on objects we'd otherwise be in the middle of polling.
    applyPendingPageChange();

    return lv_timer_handler();
}

}  // namespace ui

#endif  // DISPLAY_SAFE_MODE
