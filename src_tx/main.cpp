// src_tx/main.cpp — ESP32-S3-Touch-AMOLED-1.75-G transmitter, step 7b.
//
// What changed from step 7:
//   - Auto-cycle is gone. Pages now change only on a horizontal swipe.
//   - CST9217 capacitive touch is wired up via lewisxhe/SensorLib's
//     TouchDrvCST92xx class. The chip's RESET line lives behind the
//     TCA9554 GPIO expander (EXIO6), so we drive that ourselves before
//     calling touch.begin(); SensorLib only sees a "no reset pin" config.
//   - A small swipe detector runs every loop tick (~200 Hz): on touch-
//     down it stamps (x0, t0); on touch-up it computes |dx| and dt. If
//     |dx| ≥ 60 px and dt < 600 ms, it emits SWIPE_LEFT or SWIPE_RIGHT.
//     Swipe-left advances to the next page, swipe-right goes back. Taps
//     and vertical strokes are ignored.
//
// What changed from step 3:
//   - The "Hello, transmitter" placeholder is gone. The AMOLED now
//     renders a multi-page status UI mirroring the receiver's layout:
//
//       1. Primary       — BLE link state, notify rate, PMU telemetry,
//                          uptime. The "you turn on the device, you
//                          look at this" page.
//       2. Simulator     — list of the 5 simulated PGN classes with
//                          their current enabled state. Read-only for
//                          step 7; step 6 will wire the RX → TX command
//                          channel to flip these.
//       3. PGN           — list of the PGNs being published, their
//                          target cadence, and their measured rate.
//       4. Settings      — placeholder. Step 7c+ will host BLE TX power,
//                          brightness, etc.
//       5. Communication — BLE-link detail page: role, advertised name,
//                          service UUID, connected-central count,
//                          per-characteristic notify counters.
//   - Black background, white/coloured text. AMOLED draws less power on
//     black than on white (each pixel is its own emitter; black = off),
//     and the colour contrast reads better in daylight too.
//   - Page content is refreshed every 500 ms from the main loop. BLE
//     callbacks (onConnect / onDisconnect) only mutate global state;
//     they never touch LVGL, because LVGL is not thread-safe and the
//     callbacks fire from NimBLE's host task.
//   - A new bleNotifyTotal counter (monotonically increasing, never
//     reset) lives alongside the existing bleNotifyCount (reset each
//     5 s heartbeat). The status display computes its per-second rate
//     from the delta between two ticks of bleNotifyTotal; the heartbeat
//     keeps using its own resetting counter. Two counters > shared
//     reset window where the heartbeat and the display fight each
//     other for "who zeros it first".
//
// What does NOT change in step 7:
//   - The BLE peripheral, the simulator, and the publish cadences are
//     untouched. Step 7 is purely an on-device UI.
//   - The receiver (src/) is still untouched — it will become a BLE
//     central in step 4 (deferred for now). The Communication page
//     mirror on the RX is part of step 4.
//
// Pin map (from the Waveshare schematic, locked in for the AMOLED-1.75-G):
//   I²C bus (AXP2101, IMU, RTC, touch, audio, expander):
//     SDA = GPIO15,  SCL = GPIO14
//   SH8601 AMOLED (QSPI):
//     CS=12, D0=4, D1=5, D2=6, D3=7, SCK=38, RESET=39, TE=13
//     Panel power = VCC3V3 directly (no AXP rail to enable).
//   Touch CST9217 (I²C 0x5a, wired up in step 7b):
//     INT=21 (unused — we poll), RESET via TCA9554 expander EXIO6
//   TCA9554 GPIO expander (I²C 0x20):
//     EXIO6 drives CST9217 RESET (others used by audio / panel — leave
//     alone via read-modify-write in tcaPinWrite()).

#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <math.h>
#include <NimBLEDevice.h>
#include <TouchDrv.hpp>
#include <BoatBle.h>

// AXP2101 lives at 7-bit I²C address 0x34. XPowersLib defines its own
// constant for the same value but we hard-code it for the standalone bus
// scan (which runs before the driver is initialised).
static constexpr uint8_t kAxp2101Addr = 0x34;

// Confirmed via step 1b probe + Waveshare schematic. The whole AMOLED-1.75
// board shares one I²C bus on these pins: AXP2101, QMI8658, PCF85063,
// CST9217 touch, TCA9554 GPIO expander, ES8311 / ES7210 audio codecs, and
// a presumed AT24Cxx EEPROM at 0x50.
static constexpr int kI2cSda = 15;
static constexpr int kI2cScl = 14;

// SH8601 AMOLED QSPI pins, from the Waveshare schematic J30 FFC connector.
// Arduino_GFX takes them in (CS, SCK, D0, D1, D2, D3) order for the QSPI
// bus and (RST) for the panel.
static constexpr int kLcdCs     = 12;
static constexpr int kLcdSck    = 38;
static constexpr int kLcdD0     = 4;
static constexpr int kLcdD1     = 5;
static constexpr int kLcdD2     = 6;
static constexpr int kLcdD3     = 7;
static constexpr int kLcdReset  = 39;
static constexpr int16_t kLcdW  = 466;
static constexpr int16_t kLcdH  = 466;

XPowersAXP2101 PMU;

// ---- Step-7 globals shared between the UI and the BLE/simulator ----------
//
// These live up here, ahead of the multi-page UI section, because
// updatePages() reads them and is defined above the BLE/simulator
// section that owns the writers. Keeping them volatile is enough —
// writes happen on NimBLE's host task (inside publishX()) and reads
// happen on the main task (in updatePages()); we only need a snapshot
// for display, not a happens-before relationship.

// Monotonic total — never reset. The UI computes its per-second rate
// from deltas between display ticks so it doesn't fight the heartbeat
// over the reset window.
static volatile uint32_t bleNotifyTotal = 0;

// Per-characteristic notify counters, for the Communication page. Updated
// inside each publishX() — gives us a "this channel is alive" view that's
// independent of subscription state.
static volatile uint32_t notifyCountWind      = 0;
static volatile uint32_t notifyCountGps       = 0;
static volatile uint32_t notifyCountHeading   = 0;
static volatile uint32_t notifyCountDepthTemp = 0;
static volatile uint32_t notifyCountAttitude  = 0;

// Sim channel enable mask. All channels enabled by default. Step 6 will
// expose this to the RX via the command characteristic; for now the Sim
// page shows it read-only.
static uint32_t simChannelMask = boatble::SIM_CH_WIND
                               | boatble::SIM_CH_GPS
                               | boatble::SIM_CH_HEADING
                               | boatble::SIM_CH_DEPTH
                               | boatble::SIM_CH_AIR_TEMP
                               | boatble::SIM_CH_ATTITUDE;

// Step 9: shared category palette, ported from the RX's honeycomb page
// (src/Ui.cpp). Four channel "families" — wind, boat-motion, GPS-derived,
// and temperature — each with a pastel background, a saturated flash
// colour (used for in-button text), and a dimmed background (unused on
// TX for now but mirrored from RX for parity).
struct HexPalette {
    uint32_t bg_hex;
    uint32_t flash_hex;
    uint32_t dim_bg_hex;
};
enum HexCat : uint8_t { CAT_WIND = 0, CAT_BOAT = 1, CAT_GPS = 2, CAT_TEMP = 3 };
static constexpr HexPalette kHexPalettes[4] = {
    /* CAT_WIND */ { 0xC6E0F5, 0x0D47A1, 0xE3F0FA },
    /* CAT_BOAT */ { 0xCFE8C9, 0x1B5E20, 0xE7F4E4 },
    /* CAT_GPS  */ { 0xDCD0EC, 0x4A148C, 0xEEE8F6 },
    /* CAT_TEMP */ { 0xF5D7B9, 0xBF360C, 0xFAEBDC },
};

// Sim-page channel definitions. Categories follow the RX clustering:
// Heading lives in the GPS cluster (HDG hex), Depth in the Boat cluster
// (DEP hex), Attitude in the Boat cluster (HEEL/PITCH/ROT). The order
// here drives both the on-screen row order and the tap → bit mapping.
struct SimChannelDef {
    const char *name;
    uint32_t    bit;
    uint8_t     cat;
};
static constexpr SimChannelDef kSimChannels[6] = {
    { "Wind",      boatble::SIM_CH_WIND,      CAT_WIND },
    { "GPS",       boatble::SIM_CH_GPS,       CAT_GPS  },
    { "Heading",   boatble::SIM_CH_HEADING,   CAT_GPS  },
    { "Depth",     boatble::SIM_CH_DEPTH,     CAT_BOAT },
    { "AirTemp",   boatble::SIM_CH_AIR_TEMP,  CAT_TEMP },
    { "Attitude",  boatble::SIM_CH_ATTITUDE,  CAT_BOAT },
};

// PGN-page channel definitions. Same category palette so the PGN page
// shows the "honeycomb feel" the user asked for (pastel-tinted text on
// black). The literal hex-tile canvas geometry from RX is a separate,
// larger port — leaving that for a follow-up.
struct PgnChannelDef {
    const char *name;
    uint8_t     cat;
};
static constexpr PgnChannelDef kPgnChannels[5] = {
    { "Wind",       CAT_WIND },
    { "GPS",        CAT_GPS  },
    { "Heading",    CAT_GPS  },
    { "Depth/Temp", CAT_TEMP },
    { "Attitude",   CAT_BOAT },
};

// Step 8: GPS state forward-declared up here so updatePages() (defined
// below in the multi-page UI section) can read it. The driver itself
// (parser, I²C poller, init routine) lives further down with the BLE
// publishers it feeds.
struct GpsState {
    char     lineBuf[120] = {0};
    int      lineLen      = 0;
    double   lat        = NAN;
    double   lon        = NAN;
    uint8_t  fixQuality = 0;
    uint8_t  numSats    = 0;
    uint32_t lastFixMs  = 0;
    // Diagnostics: most recent complete NMEA line + running byte counters
    // for the heartbeat. lastLine is updated on every '\n'. Bytes from
    // I²C and UART are counted separately so we can see which path is
    // actually feeding the parser on a given board.
    char     lastLine[120] = {0};
    uint32_t i2cBytes      = 0;
    uint32_t uartBytes     = 0;
    uint32_t linesParsed   = 0;
};
static GpsState gps;
static constexpr uint32_t kGpsFixStaleMs = 5000;

static bool gpsHasFreshFix(uint32_t now) {
    return gps.fixQuality >= 1
        && (now - gps.lastFixMs) < kGpsFixStaleMs;
}

// Allocated in initDisplay(). Kept as globals because Arduino_GFX is built
// around long-lived objects (the bus owns the SPI peripheral handle, the
// panel owns the framebuffer pump). Easier than tracking ownership.
Arduino_DataBus *gfxBus = nullptr;
Arduino_GFX     *gfx    = nullptr;

// Bring up the SH8601 panel via Arduino_GFX. We no longer paint a colour
// here — LVGL takes over once initLvgl() runs and draws the actual UI.
static bool initDisplay() {
    Serial.println("Initialising SH8601 AMOLED (Arduino_GFX over QSPI)...");
    Serial.printf("  Pins: CS=%d SCK=%d D0=%d D1=%d D2=%d D3=%d RESET=%d\r\n",
                  kLcdCs, kLcdSck, kLcdD0, kLcdD1, kLcdD2, kLcdD3, kLcdReset);
    Serial.printf("  Panel: %d × %d\r\n", kLcdW, kLcdH);

    gfxBus = new Arduino_ESP32QSPI(kLcdCs, kLcdSck,
                                    kLcdD0, kLcdD1, kLcdD2, kLcdD3);
    // Use Arduino_CO5300 even though the factory boot log reports the
    // controller as "sh8601". The SH8601 and CO5300 panels are functionally
    // identical at the QSPI command-set level. Arduino_GFX 1.4.x ships an
    // Arduino_CO5300 driver but no SH8601; the SH8601 class only landed in
    // 1.5.x (which requires Arduino-ESP32 3.x).
    // Trailing (col_offset1, row_offset1, col_offset2, row_offset2) shift the
    // visible window inside the controller's pixel RAM. The CO5300 chip can
    // address slightly wider/taller regions than this 466×466 panel actually
    // displays. Without an offset, writes land 6 columns to the left of the
    // visible region, leaving the rightmost 6 columns showing uninitialised
    // RAM (the green seam observed in step 1c-iii). 6 is the standard value
    // for these round 466×466 AMOLEDs from Waveshare / LilyGo.
    gfx = new Arduino_CO5300(gfxBus, kLcdReset, 0 /*rotation*/, false /*ips*/,
                              kLcdW, kLcdH,
                              6 /*col_offset1*/, 0 /*row_offset1*/,
                              6 /*col_offset2*/, 0 /*row_offset2*/);

    if (!gfx->begin()) {
        Serial.println("  ERROR: gfx->begin() returned false. AMOLED stays on");
        Serial.println("  the previous frame. Check pin assignments + reset");
        Serial.println("  wiring; verify VCC3V3 is reaching J30 pins 22/23.");
        return false;
    }
    Serial.println("  begin() OK");
    // Clear to black so the first LVGL frame appears on a clean background
    // rather than fading in on top of the previous firmware's leftovers.
    gfx->fillScreen(BLACK);
    return true;
}

// ---- LVGL plumbing --------------------------------------------------------
//
// Single full-size RGB565 buffer in PSRAM (466 × 466 × 2 B ≈ 434 KB). One
// buffer is fine for this display rate — LVGL will redraw only the dirty
// area each tick. The flush callback is synchronous-blocking; refresh time
// is dominated by the QSPI write (~50 ms full-screen at 80 MHz). DMA can
// come later if the UI ever needs it.
static lv_disp_draw_buf_t lvDrawBuf;
static lv_color_t        *lvBuf      = nullptr;
static lv_disp_drv_t      lvDispDrv;

static void flushDisplayCb(lv_disp_drv_t *disp,
                           const lv_area_t *area,
                           lv_color_t *color_p) {
    const int16_t w = area->x2 - area->x1 + 1;
    const int16_t h = area->y2 - area->y1 + 1;
    // lv_color_t at LV_COLOR_DEPTH=16 is a 16-bit RGB565 value. Arduino_GFX
    // expects the same in-memory layout for draw16bitRGBBitmap. If colours
    // come out swapped (e.g. white shows up as cyan) we'd need to flip
    // LV_COLOR_16_SWAP in the shared lv_conf.h, but the RX uses the same
    // setting and renders correctly, so this should "just work".
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(disp);
}

// Bring up LVGL: allocate the draw buffer in PSRAM, register the display
// driver, then create a centred "Hello, transmitter" label. Returns false
// if PSRAM allocation fails (everything else here is infallible at this
// stage).
static bool initLvgl() {
    Serial.println("Initialising LVGL...");
    lv_init();

    const size_t pixels = (size_t)kLcdW * kLcdH;
    lvBuf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM);
    if (!lvBuf) {
        Serial.println("  ERROR: PSRAM alloc for LVGL buffer failed.");
        return false;
    }
    Serial.printf("  Allocated %u-byte draw buffer in PSRAM.\r\n",
                  (unsigned)(pixels * sizeof(lv_color_t)));

    lv_disp_draw_buf_init(&lvDrawBuf, lvBuf, nullptr, pixels);
    lv_disp_drv_init(&lvDispDrv);
    lvDispDrv.hor_res  = kLcdW;
    lvDispDrv.ver_res  = kLcdH;
    lvDispDrv.flush_cb = flushDisplayCb;
    lvDispDrv.draw_buf = &lvDrawBuf;
    // Note (2026-05-16): the firmware's native orientation puts LVGL's
    // top edge at the *physical right side of the disc with USB-C on the
    // right*. This is the intended hold position (USB-right). Earlier
    // sw_rotate experiments tried to compensate for what looked like a
    // sideways display in photos taken with USB-top — that turned out to
    // be the user holding the device in a non-native orientation for the
    // photo, not a real bug. No software rotation needed.
    lv_disp_drv_register(&lvDispDrv);
    Serial.println("  LVGL display driver registered.");

    // Black background everywhere — AMOLED-friendly and easier to read in
    // a wiring locker. Pages are built and shown in initPages().
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    Serial.println("  LVGL ready; page setup deferred to initPages().");
    return true;
}

// ---- Multi-page status UI -------------------------------------------------
//
// Five pages, one shown at a time, swiped left/right to change. Layout
// per page:
//   y=40   title (font_20, dim-blue)
//   y=90+  content (font_14 by default)
// Pages cover the full 466×466 screen, no border, black background. Only
// the visible page has its LV_OBJ_FLAG_HIDDEN cleared; the others are
// hidden but kept in memory so we don't pay a build cost on each switch.

enum TxPage : uint8_t {
    TXP_PRIMARY    = 0,
    TXP_SIMULATOR  = 1,
    TXP_PGN        = 2,
    TXP_SETTINGS   = 3,
    TXP_COMM       = 4,
    TXP_COUNT      = 5,
};
static const char *kPageTitles[TXP_COUNT] = {
    "Primary",
    "Simulator",
    "PGN",
    "Settings",
    "Communication",
};
static TxPage     currentPage  = TXP_PRIMARY;
static lv_obj_t  *pageRoots[TXP_COUNT] = {};

// Dynamic labels we update each tick. Grouped per page so updaters can
// access them by member name rather than juggling unnamed indices.
static struct {
    lv_obj_t *ble_state;
    lv_obj_t *rate;
    lv_obj_t *gps;        // step 8 — "GPS: N sats / fix-type"
    lv_obj_t *vbus;
    lv_obj_t *battery;
    lv_obj_t *uptime;
} primaryPg;

// Step 9: each sim row is a tappable pill — root container with a label
// inside. The bg colour reflects whether the channel is currently
// enabled in simChannelMask (category pastel when on, dim grey when off).
// y_top / y_bot let pollTouch tap-hit-test back to a row index.
struct SimRow {
    lv_obj_t *root  = nullptr;
    lv_obj_t *lbl   = nullptr;
    int16_t   y_top = 0;
    int16_t   y_bot = 0;
};
static SimRow simPg[6];

static struct {
    lv_obj_t *rows[5];  // one per published PGN
} pgnPg;

static struct {
    lv_obj_t *role;
    lv_obj_t *name;
    lv_obj_t *service;
    lv_obj_t *clients;
    lv_obj_t *total;
} commPg;

// Build a full-screen page with a title label. Returns the page root; the
// caller adds content below the title.
static lv_obj_t *makePage(const char *title) {
    lv_obj_t *p = lv_obj_create(lv_scr_act());
    lv_obj_set_size(p, kLcdW, kLcdH);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(p, 0, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_make(0x4f, 0xc3, 0xf7), LV_PART_MAIN);  // light blue
    // Step 9: 20 -> 28 pt for the title. The smaller font's AA edges fought
    // the AMOLED's subpixel layout and read as italic on certain glyphs.
    // y=40 keeps the "Communication" title (the widest one) inside the
    // round panel's chord at that height (≈260 px visible width).
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);
    return p;
}

// Add a content label inside a page. Centred horizontally, white-on-black.
// Step 8 bumped 14 -> 20. Step 9 bumps 20 -> 24 — the 20 still showed
// italic-ish artefacts on the live-updating Notifies and rate counters.
static lv_obj_t *makeRow(lv_obj_t *page, int y) {
    lv_obj_t *l = lv_label_create(page);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// Make a row visible only on the currently-active page.
static void showPage(TxPage p) {
    for (uint8_t i = 0; i < TXP_COUNT; i++) {
        if (i == p) {
            lv_obj_clear_flag(pageRoots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pageRoots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    currentPage = p;
}

static void initPages() {
    Serial.println("Building status pages...");

    // --- Primary --------------------------------------------------------
    pageRoots[TXP_PRIMARY] = makePage(kPageTitles[TXP_PRIMARY]);
    {
        lv_obj_t *p = pageRoots[TXP_PRIMARY];
        // 40 px row pitch for the 24 pt font (rows are ~30 px tall, so this
        // leaves a clean 10 px gap). Slightly wider gaps separate the
        // BLE/GPS group from the power group from uptime.
        primaryPg.ble_state = makeRow(p,  90);
        primaryPg.rate      = makeRow(p, 130);
        primaryPg.gps       = makeRow(p, 170);
        primaryPg.vbus      = makeRow(p, 220);
        primaryPg.battery   = makeRow(p, 260);
        primaryPg.uptime    = makeRow(p, 315);
    }

    // --- Simulator ------------------------------------------------------
    pageRoots[TXP_SIMULATOR] = makePage(kPageTitles[TXP_SIMULATOR]);
    {
        lv_obj_t *p = pageRoots[TXP_SIMULATOR];
        // Six pill-shaped tappable rows. The exact pixel geometry has to
        // sit inside the round 466 px viewport — start at y=85 and step
        // 44 px so the last row ends at y=85+5*44+38=343, well clear of
        // the 466 px bottom and the round bezel cropping.
        const int16_t btn_w  = 280;
        const int16_t btn_h  = 38;
        const int16_t btn_x  = (kLcdW - btn_w) / 2;
        const int16_t pitch  = 44;
        const int16_t y0     = 85;
        for (int i = 0; i < 6; i++) {
            const int16_t y = y0 + i * pitch;
            lv_obj_t *btn = lv_obj_create(p);
            lv_obj_set_size(btn, btn_w, btn_h);
            lv_obj_set_pos(btn, btn_x, y);
            lv_obj_set_style_radius(btn, 19, LV_PART_MAIN);
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            // Mark the button as non-clickable from LVGL's POV — taps are
            // routed through our pollTouch hit-test, not the LVGL indev
            // tree (which we don't register).
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
            lv_label_set_text(lbl, kSimChannels[i].name);
            lv_obj_center(lbl);

            simPg[i].root  = btn;
            simPg[i].lbl   = lbl;
            simPg[i].y_top = y;
            simPg[i].y_bot = y + btn_h;
        }
    }

    // --- PGN ------------------------------------------------------------
    pageRoots[TXP_PGN] = makePage(kPageTitles[TXP_PGN]);
    {
        lv_obj_t *p = pageRoots[TXP_PGN];
        // Five rows at 45 px pitch — wider than the Sim pitch because the
        // PGN labels (rate + name) are longer strings and benefit from
        // extra breathing room.
        for (int i = 0; i < 5; i++) {
            pgnPg.rows[i] = makeRow(p, 100 + i * 45);
            // Tint by category — pastel text on black for the
            // "honeycomb feel" without the actual hex canvas.
            const auto &pal = kHexPalettes[kPgnChannels[i].cat];
            lv_obj_set_style_text_color(pgnPg.rows[i],
                lv_color_hex(pal.bg_hex), LV_PART_MAIN);
        }
    }

    // --- Settings (placeholder) ----------------------------------------
    pageRoots[TXP_SETTINGS] = makePage(kPageTitles[TXP_SETTINGS]);
    {
        lv_obj_t *p = pageRoots[TXP_SETTINGS];
        lv_obj_t *l = makeRow(p, 200);
        lv_label_set_text(l, "(no settings yet)");
        lv_obj_set_style_text_color(l, lv_color_make(0x9e, 0x9e, 0x9e),
                                    LV_PART_MAIN);
    }

    // --- Communication --------------------------------------------------
    pageRoots[TXP_COMM] = makePage(kPageTitles[TXP_COMM]);
    {
        lv_obj_t *p = pageRoots[TXP_COMM];
        // 42 px pitch for the role/name/service block (24 pt font);
        // wider 55 px gap before the live-updating clients/total
        // counters so the eye groups them apart.
        commPg.role    = makeRow(p,  90);
        commPg.name    = makeRow(p, 132);
        commPg.service = makeRow(p, 174);
        commPg.clients = makeRow(p, 230);
        commPg.total   = makeRow(p, 272);
    }

    // Static content that never changes once boot is done.
    lv_label_set_text(commPg.role, "Role: Transmitter");
    char buf[64];
    snprintf(buf, sizeof(buf), "Name: %s", BOAT_BLE_DEVICE_NAME);
    lv_label_set_text(commPg.name, buf);
    snprintf(buf, sizeof(buf), "Service: ...%s",
             BOAT_BLE_SERVICE_UUID + 32);  // last 4 chars: "9f01"
    lv_label_set_text(commPg.service, buf);

    showPage(TXP_PRIMARY);
    Serial.println("  Pages built; showing Primary.");
}

// Refresh the currently-visible page from the live state globals. Called
// every kUiUpdatePeriodMs from loop() — never from BLE callbacks (LVGL is
// not thread-safe across the main task and the NimBLE host task).
static void updatePages(uint32_t now, uint32_t connectedCount, uint32_t ratePerSec) {
    char buf[80];
    switch (currentPage) {
        case TXP_PRIMARY: {
            snprintf(buf, sizeof(buf), "BLE: %s",
                     connectedCount > 0 ? "client connected" : "advertising");
            lv_label_set_text(primaryPg.ble_state, buf);

            snprintf(buf, sizeof(buf), "rate: %u msg/s", (unsigned)ratePerSec);
            lv_label_set_text(primaryPg.rate, buf);

            // Step 8 — GPS state. Shows sats + fix-quality; "(sim)" suffix
            // when the BLE GPS values are still falling back to the fake
            // Aarhus coordinates, so the user knows where lat/lon comes from.
            const char *fixName = "no fix";
            switch (gps.fixQuality) {
                case 1: fixName = "GPS";     break;
                case 2: fixName = "DGPS";    break;
                case 4: fixName = "RTK fix"; break;
                case 5: fixName = "RTK flt"; break;
                case 6: fixName = "deadrec"; break;
                default: break;
            }
            if (gpsHasFreshFix(now)) {
                snprintf(buf, sizeof(buf), "GPS: %u sats / %s",
                         (unsigned)gps.numSats, fixName);
            } else if (gps.numSats > 0) {
                snprintf(buf, sizeof(buf), "GPS: %u sats / %s (sim)",
                         (unsigned)gps.numSats, fixName);
            } else {
                snprintf(buf, sizeof(buf), "GPS: no signal (sim)");
            }
            lv_label_set_text(primaryPg.gps, buf);

            snprintf(buf, sizeof(buf), "VBUS: %s",
                     PMU.isVbusIn() ? "connected" : "—");
            lv_label_set_text(primaryPg.vbus, buf);

            uint16_t v = PMU.getBattVoltage();
            if (PMU.isBatteryConnect() && v > 0) {
                snprintf(buf, sizeof(buf), "Battery: %u mV (%d%%)",
                         (unsigned)v, PMU.getBatteryPercent());
            } else {
                snprintf(buf, sizeof(buf), "Battery: not present");
            }
            lv_label_set_text(primaryPg.battery, buf);

            uint32_t sec = now / 1000;
            snprintf(buf, sizeof(buf), "up: %02u:%02u:%02u",
                     (unsigned)(sec / 3600),
                     (unsigned)((sec / 60) % 60),
                     (unsigned)(sec % 60));
            lv_label_set_text(primaryPg.uptime, buf);
            break;
        }

        case TXP_SIMULATOR: {
            // One pill per channel. Background uses the category pastel
            // when the bit is set in simChannelMask; otherwise a dim
            // grey. Text uses the corresponding flash colour on top of
            // the pastel for high contrast, and a lighter grey on the
            // dim background.
            for (int i = 0; i < 6; i++) {
                const bool on  = (simChannelMask & kSimChannels[i].bit) != 0;
                const auto &pal = kHexPalettes[kSimChannels[i].cat];
                lv_obj_set_style_bg_opa(simPg[i].root,
                    LV_OPA_COVER, LV_PART_MAIN);
                if (on) {
                    lv_obj_set_style_bg_color(simPg[i].root,
                        lv_color_hex(pal.bg_hex), LV_PART_MAIN);
                    lv_obj_set_style_text_color(simPg[i].lbl,
                        lv_color_hex(pal.flash_hex), LV_PART_MAIN);
                } else {
                    lv_obj_set_style_bg_color(simPg[i].root,
                        lv_color_make(0x21, 0x21, 0x21), LV_PART_MAIN);
                    lv_obj_set_style_text_color(simPg[i].lbl,
                        lv_color_make(0x75, 0x75, 0x75), LV_PART_MAIN);
                }
            }
            break;
        }

        case TXP_PGN: {
            // One row per PGN: short name + measured Hz. Each row's text
            // colour is set once in initPages() from kPgnChannels, so we
            // don't restate it here.
            static uint32_t lastCounts[5] = {0};
            static uint32_t lastTickMs    = 0;
            uint32_t curCounts[5] = {
                notifyCountWind, notifyCountGps, notifyCountHeading,
                notifyCountDepthTemp, notifyCountAttitude,
            };
            uint32_t dtMs = (lastTickMs == 0) ? 1000 : (now - lastTickMs);
            if (dtMs == 0) dtMs = 1;
            for (int i = 0; i < 5; i++) {
                uint32_t delta = curCounts[i] - lastCounts[i];
                float hz = (delta * 1000.0f) / dtMs;
                snprintf(buf, sizeof(buf), "%-12s %4.1f Hz",
                         kPgnChannels[i].name, hz);
                lv_label_set_text(pgnPg.rows[i], buf);
                lastCounts[i] = curCounts[i];
            }
            lastTickMs = now;
            break;
        }

        case TXP_SETTINGS:
            // Static placeholder — nothing to refresh.
            break;

        case TXP_COMM: {
            snprintf(buf, sizeof(buf), "Clients: %u", (unsigned)connectedCount);
            lv_label_set_text(commPg.clients, buf);

            snprintf(buf, sizeof(buf), "Notifies: %lu",
                     (unsigned long)bleNotifyTotal);
            lv_label_set_text(commPg.total, buf);
            break;
        }

        default:
            break;
    }
}

// ---- TCA9554 GPIO expander helper -----------------------------------------
//
// The CST9217 RESET line is not wired to a GPIO on the ESP32-S3 — it's
// behind the TCA9554 expander at I²C address 0x20, on pin EXIO6. We need
// the helper for the touch chip's power-up reset pulse; we keep it tiny
// (read-modify-write so we don't clobber other EXIO pins potentially used
// by the audio codec / panel / etc.).

static constexpr uint8_t kTca9554Addr  = 0x20;
static constexpr uint8_t kTcaRegOutput = 0x01;
static constexpr uint8_t kTcaRegConfig = 0x03;

static bool tcaReadReg(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(kTca9554Addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(kTca9554Addr, (uint8_t)1) != 1) return false;
    *val = Wire.read();
    return true;
}

static bool tcaWriteReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(kTca9554Addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// Drive EXIO<pin> as a push-pull output at the requested level. Reads the
// current state of both registers first so other expander pins stay put.
static bool tcaPinWrite(uint8_t pin, bool high) {
    uint8_t cfg = 0, out = 0;
    if (!tcaReadReg(kTcaRegConfig, &cfg)) return false;
    if (!tcaReadReg(kTcaRegOutput, &out)) return false;
    cfg &= ~(1 << pin);                      // 0 in the config reg = output
    if (high) out |= (1 << pin); else out &= ~(1 << pin);
    if (!tcaWriteReg(kTcaRegConfig, cfg)) return false;
    if (!tcaWriteReg(kTcaRegOutput, out)) return false;
    return true;
}

// ---- CST9217 touch + swipe nav --------------------------------------------
//
// The CST9217 is an oddball — it uses 16-bit register addresses (big-
// endian) where most CST-family chips use 8-bit, so generic CST816-style
// drivers don't work. lewisxhe/SensorLib has a TouchDrvCST92xx class that
// handles this correctly; we just feed it the I²C bus and read points.
//
// We don't use the INT line (GPIO21). Polling at the loop rate (~200 Hz
// because of the trailing delay(5)) is plenty for swipe detection and is
// simpler than wiring up an interrupt handler that has to be careful
// about I²C-from-ISR.

static constexpr uint8_t kCstAddr      = 0x5a;
static constexpr uint8_t kCstResetExio = 6;     // TCA9554 EXIO6 → RESET

static TouchDrvCST92xx touch;

// Swipe detector state. A "swipe" is a touch-down → touch-up sequence
// with horizontal travel ≥ kSwipeMinDx and total duration < kSwipeMaxMs.
// Anything slower is treated as a drag (ignored). Vertical strokes are
// ignored too — touch nav is left/right only.
//
// Thresholds tuned 2026-05-12 against real swipes through the AMOLED
// stack: the panel's glass + capacitive shield seems to swallow a chunk
// of each gesture, so 60 px / 600 ms felt strict in practice. 40 px is
// still ~9% of the 466 px screen — small enough that a stationary tap
// can't trigger it, large enough that no realistic page-flip gesture
// fails to clear it.
static constexpr int16_t  kSwipeMinDx     = 40;
static constexpr uint32_t kSwipeMaxMs     = 1000;
// After firing a swipe we ignore further touch state for this window.
// The CST9217 occasionally drops points mid-stroke (chip reports zero
// points for one or two ticks while the finger is still down), and
// without a lockout we'd see touch-down → drop → touch-up → swipe →
// re-touch-down (same finger, no actual lift) → touch-up → swipe again.
// 300 ms is long enough to cover any realistic mid-stroke gap and
// short enough that intentional repeat swipes still feel responsive.
static constexpr uint32_t kSwipeLockoutMs = 300;

static bool     touchDown       = false;
static int16_t  touchStartX     = 0;
static int16_t  touchStartY     = 0;
static int16_t  touchLastX      = 0;
static int16_t  touchLastY      = 0;
static uint32_t touchStartMs    = 0;
static uint32_t swipeLockoutMs  = 0;           // millis() value to wait until
static bool     touchReady      = false;       // true once initTouch() succeeded

enum SwipeDir : int8_t {
    SWIPE_NONE  = 0,
    SWIPE_LEFT  = -1,
    SWIPE_RIGHT = 1,
};

static bool initTouch() {
    Serial.println("Initialising CST9217 touch (via SensorLib)...");

    // Reset pulse via TCA9554 EXIO6. CST9217 datasheet calls for ≥10 ms
    // low and ≥50 ms high before the chip is reachable on I²C — we give
    // it a generous margin on both sides.
    if (!tcaPinWrite(kCstResetExio, false)) {
        Serial.println("  ERROR: TCA9554 write (RESET low) failed");
        return false;
    }
    delay(20);
    if (!tcaPinWrite(kCstResetExio, true)) {
        Serial.println("  ERROR: TCA9554 write (RESET high) failed");
        return false;
    }
    delay(60);

    // We've already pulsed RESET ourselves and we don't use the INT line,
    // so pass -1 for both pins — the library will skip GPIO config and
    // operate in pure-polling mode.
    touch.setPins(-1, -1);
    if (!touch.begin(Wire, kCstAddr, kI2cSda, kI2cScl)) {
        Serial.println("  ERROR: touch.begin() failed");
        return false;
    }
    Serial.printf("  Model: %s\r\n", touch.getModelName());
    // No coordinate transform is applied. CST9217 reports raw panel
    // coordinates that already match LVGL's coordinate system in the
    // firmware's native orientation (USB-right). Confirmed empirically:
    // a tap on LVGL's Heading pill at y=173 reads chip_y in the same
    // range; a tap on AirTemp at y=261 reads chip_y ≈ 261; etc. If you
    // hold the device with USB at the top, the touch will *feel*
    // inverted because the screen looks rotated 90° — but that's a
    // hold-orientation issue, not a chip-orientation issue.
    return true;
}

// Poll the chip once. Returns SWIPE_LEFT / SWIPE_RIGHT exactly once when
// a touch-up event closes a qualifying gesture; returns SWIPE_NONE while
// the finger is down or after a non-qualifying stroke.
static SwipeDir pollTouch(uint32_t now) {
    if (!touchReady) return SWIPE_NONE;

    // Post-swipe quiet window: skip the I²C read entirely so we don't
    // re-arm on chip glitches firing right after a real swipe.
    if (swipeLockoutMs != 0 && now < swipeLockoutMs) {
        return SWIPE_NONE;
    }

    TouchPoints pts = touch.getTouchPoints();
    // Gate on getPointCount(), NOT hasPoints(): SensorLib's hasPoints()
    // returns true for any chip activity, including events that don't
    // carry valid coordinates (proximity, cover, partial reports). On
    // those events getPoint(0) logs "Invalid touch point index: 0" and
    // returns a zeroed point — which would corrupt touchStartX into 0
    // and make every later real touch read as a giant swipe.
    if (pts.getPointCount() > 0) {
        // First finger is the only one we care about — we treat
        // multitouch as just the primary contact for swipe purposes.
        const TouchPoint &p = pts.getPoint(0);
        touchLastX = p.x;
        touchLastY = p.y;
        if (!touchDown) {
            touchDown    = true;
            touchStartX  = p.x;
            touchStartY  = p.y;
            touchStartMs = now;
        }
        return SWIPE_NONE;
    }

    // No finger present this tick. If we were tracking one, this is the
    // touch-up edge — measure what we got and emit a swipe or tap if it
    // qualifies.
    if (touchDown) {
        touchDown = false;
        const int32_t  dx = (int32_t)touchLastX - (int32_t)touchStartX;
        const int32_t  dy = (int32_t)touchLastY - (int32_t)touchStartY;
        const uint32_t dt = now - touchStartMs;
        if (dt < kSwipeMaxMs && abs(dx) >= kSwipeMinDx) {
            // Arm the lockout so a mid-stroke point drop on the *next*
            // physical gesture doesn't slip past us.
            swipeLockoutMs = now + kSwipeLockoutMs;
            // dx < 0 → finger moved left → next page (the current page
            // slides away to the left, just like iOS/Android).
            return dx < 0 ? SWIPE_LEFT : SWIPE_RIGHT;
        }
        (void)dy;
    }
    return SWIPE_NONE;
}

// ---- LC76G GNSS -----------------------------------------------------------
//
// I²C-mode driver for the Quectel LC76G on this AMOLED-1.75-G board.
//
// Protocol (from the espressif issue #10731 + Quectel forum thread #27337):
//   * The chip uses TWO I²C addresses: 0x50 for writes, 0x54 for reads.
//   * Address 0x58 also exists but we don't use it. Quectel's app note
//     describes it as a "buffer state" read; 0x54 alone is enough for the
//     unsolicited NMEA stream.
//   * To poll for new NMEA bytes:
//       1. Write a "length query" packet to 0x50.
//       2. Read 4 bytes (little-endian uint32) from 0x54.
//       3. If length > 0, write a "data fetch" packet to 0x50 with the
//          length, then read `length` bytes from 0x54.
//
// We parse $G?GGA sentences for lat / lon / fix-quality / sat-count. RMC
// (for COG / SOG) is left to a follow-up — the simulator already covers
// those fields and the Primary page only shows sats+fix.
//
// RESET line: TCA9554 EXIO7. Defaults to "chip running" when EXIO7 floats
// because the LC76G has an internal pull-up on RESET, but we drive it
// explicitly during init for a clean power-up state.

static constexpr uint8_t kLc76gCmdAddr    = 0x50;
// Step 8c experiment: try reading from 0x50 (same as the command address)
// instead of 0x54. The boot scan only saw 0x50 ACKing — 0x54 and 0x58
// never appeared — so the working protocol on this board may multiplex
// commands and responses on a single address. If this works we keep it;
// if it doesn't we'll see i2cRead -1 same as before and revisit.
static constexpr uint8_t kLc76gReadAddr   = 0x50;
static constexpr uint8_t kLc76gResetExio  = 7;

// LC76G UART path. The chip's TXD1 goes to GPIO17 via R15 (NC/0R jumper)
// and RXD1 to GPIO18 via R16. On boards where Waveshare populated the
// jumpers, we get a free NMEA stream over UART at the chip default
// 9600 baud. On boards where they didn't, Serial1 reads nothing — same
// failure mode as I²C, so it's cheap to try.
static constexpr int kGpsUartRx   = 17;
static constexpr int kGpsUartTx   = 18;
static constexpr uint32_t kGpsUartBaud = 9600;

// Length-query packet: ask "how many bytes do you have for me?". The first
// byte is the LC76G's internal register pointer (0x08 = "command in"); the
// next four bytes are the magic prefix + command + length-of-data-being-
// sent (here 4 because the response will be a 4-byte little-endian count).
static const uint8_t kLc76gLenQuery[] = {
    0x08, 0x00, 0x51, 0xaa, 0x04, 0x00, 0x00, 0x00,
};

// (GpsState struct, `gps` global, kGpsFixStaleMs, and gpsHasFreshFix() are
//  forward-declared near the top of the file so updatePages() can read
//  them. See the Step-8-globals block right after the PMU declaration.)

// Parse one decimal-minutes NMEA coordinate ("ddmm.mmmm" or "dddmm.mmmm")
// into signed decimal degrees. `hemi` is the N/S/E/W character.
static double parseNmeaCoord(const char *s, char hemi) {
    if (!s || !*s) return NAN;
    double raw = atof(s);
    double deg = floor(raw / 100.0);
    double min = raw - deg * 100.0;
    double dec = deg + min / 60.0;
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
}

// Validate NMEA checksum: XOR of bytes between '$' and '*' equals the two
// hex digits after the '*'. Returns true if valid.
static bool nmeaChecksumOk(const char *line, int len) {
    int star = -1;
    for (int i = 1; i < len; i++) if (line[i] == '*') { star = i; break; }
    if (star < 0 || star + 3 > len) return false;
    uint8_t sum = 0;
    for (int i = 1; i < star; i++) sum ^= (uint8_t)line[i];
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int hi = hex(line[star + 1]);
    int lo = hex(line[star + 2]);
    if (hi < 0 || lo < 0) return false;
    return (uint8_t)((hi << 4) | lo) == sum;
}

// Field iterator. `start` and `end` point into `line` (the '*' before
// checksum is treated as end-of-fields). nextField() returns a NUL-
// terminated pointer to the next field by overwriting commas. Mutates
// the buffer; caller must pass a writable copy.
static const char *nextField(char **cursor) {
    if (!*cursor) return nullptr;
    char *start = *cursor;
    char *p = start;
    while (*p && *p != ',' && *p != '*') p++;
    char term = *p;
    *p = '\0';
    *cursor = (term == ',') ? (p + 1) : nullptr;
    return start;
}

// Parse one full NMEA line ('\r' and '\n' already stripped). Dispatches
// on the talker+type prefix. Mutates `line` in place during parsing.
static void parseNmeaLine(char *line, int len, uint32_t now) {
    if (len < 7 || line[0] != '$') return;
    if (!nmeaChecksumOk(line, len)) return;

    // Match "$??GGA" — any talker prefix is OK (GP, GN, GL, GA, BD, ...).
    if (!(line[3] == 'G' && line[4] == 'G' && line[5] == 'A')) return;

    // Skip the talker+type, start at the first comma.
    char *cursor = line + 6;
    if (*cursor != ',') return;
    cursor++;

    const char *time_s   = nextField(&cursor);  // hhmmss.sss
    const char *lat_s    = nextField(&cursor);  // ddmm.mmmm
    const char *latH_s   = nextField(&cursor);  // N/S
    const char *lon_s    = nextField(&cursor);  // dddmm.mmmm
    const char *lonH_s   = nextField(&cursor);  // E/W
    const char *fix_s    = nextField(&cursor);  // 0..6
    const char *sats_s   = nextField(&cursor);  // count
    (void)time_s;
    if (!fix_s || !sats_s) return;

    gps.fixQuality = (uint8_t)atoi(fix_s);
    gps.numSats    = (uint8_t)atoi(sats_s);
    if (gps.fixQuality >= 1 && lat_s && latH_s && lon_s && lonH_s) {
        gps.lat = parseNmeaCoord(lat_s, latH_s[0]);
        gps.lon = parseNmeaCoord(lon_s, lonH_s[0]);
        gps.lastFixMs = now;
    }
}

// Feed one byte from the I²C read stream into the line assembler.
static void gpsFeedByte(uint8_t b, uint32_t now) {
    if (b == '\n') {
        if (gps.lineLen > 0) {
            // Strip trailing '\r' if present.
            if (gps.lineBuf[gps.lineLen - 1] == '\r') gps.lineLen--;
            gps.lineBuf[gps.lineLen] = '\0';
            // Save a copy for the heartbeat diagnostic before parsing.
            const size_t cap = sizeof(gps.lastLine) - 1;
            const size_t n   = (size_t)gps.lineLen < cap ? (size_t)gps.lineLen : cap;
            memcpy(gps.lastLine, gps.lineBuf, n);
            gps.lastLine[n] = '\0';
            gps.linesParsed++;
            parseNmeaLine(gps.lineBuf, gps.lineLen, now);
        }
        gps.lineLen = 0;
        return;
    }
    if (gps.lineLen >= (int)sizeof(gps.lineBuf) - 1) {
        // Overrun — discard partial line and resync on the next '\n'.
        gps.lineLen = 0;
        return;
    }
    gps.lineBuf[gps.lineLen++] = (char)b;
}

// Step 8b: counts down on each poll; the first N polls log every step so
// we can localise where the protocol is breaking. Set back to 0 once GPS
// is healthy to keep the heartbeat quiet. Bumped 3 -> 5 for step 8c so
// we see enough samples of the new 0x50-read experiment.
static int gpsDbgPollsLeft = 5;

// Step 8 conclusion: the AMOLED-1.75-G's LC76G doesn't expose a working
// I²C streaming path without the Quectel application note we couldn't
// obtain. Reads from 0x50 return four zero bytes once then NACK; reads
// from 0x54 / 0x58 never ACK. Both the address-pair protocol (#10731 on
// espressif/arduino-esp32) and a single-address 0x50 read were tried.
//
// Initialising gpsI2cFails to kGpsI2cMaxFails effectively disables the
// I²C poll at boot — every gpsPoll() short-circuits at the first check
// and no Wire calls fire, so the log stays clean. The UART drain in
// loop() still runs and will pick up NMEA the moment R15 / R16 jumpers
// are soldered. To re-enable I²C polling for a future experiment, zero
// gpsI2cFails (e.g. via `gpsI2cFails = 0;` at the bottom of initGps).
static constexpr int kGpsI2cMaxFails = 30;
static int gpsI2cFails = kGpsI2cMaxFails;   // start "given up"

// Forward decl — defined further down with initGps(), but referenced by
// gpsPoll() on every error path for recovery.
static void gpsUnstick();

// Poll the chip once. Returns the number of bytes consumed (for logging).
static int gpsPoll(uint32_t now) {
    if (gpsI2cFails >= kGpsI2cMaxFails) return 0;   // gave up
    const bool verbose = gpsDbgPollsLeft > 0;
    if (verbose) gpsDbgPollsLeft--;

    // 1. Length query.
    Wire.beginTransmission(kLc76gCmdAddr);
    Wire.write(kLc76gLenQuery, sizeof(kLc76gLenQuery));
    const uint8_t r1 = Wire.endTransmission();
    if (verbose) Serial.printf("[gps-dbg] len-query write -> %u\r\n", r1);
    if (r1 != 0) { gpsUnstick(); gpsI2cFails++; return -1; }

    // Quectel's app note / working bring-up code from the espressif issue
    // (#10731) delays ~100 ms here for the chip to compute its response.
    // Without this delay we sometimes read stale or empty length bytes.
    delay(50);

    // 2. Read 4-byte little-endian length straight from 0x54. Earlier the
    // code prefixed this with a write of register-pointer 0x00 (matching
    // the espressif issue #10731 example). That fails on this board with
    // i2cWriteReadNonStop -1 — the chip NACKs the read address after a
    // repeated-start. The Quectel forum unstick hint ("read 1 byte from
    // 0x54 and 0x58") implies bare requestFrom is the supported pattern:
    // 0x54 is a streaming response channel, not a register-mapped device.
    uint8_t got = Wire.requestFrom(kLc76gReadAddr, (uint8_t)4);
    if (verbose) Serial.printf("[gps-dbg] req 4 from 0x54 -> %u\r\n", got);
    if (got != 4) { gpsUnstick(); gpsI2cFails++; return -1; }
    uint8_t lb[4];
    for (int i = 0; i < 4; i++) lb[i] = (uint8_t)Wire.read();
    uint32_t avail = (uint32_t)lb[0]
                   | ((uint32_t)lb[1] << 8)
                   | ((uint32_t)lb[2] << 16)
                   | ((uint32_t)lb[3] << 24);
    if (verbose) {
        Serial.printf("[gps-dbg] len bytes: %02x %02x %02x %02x -> %lu\r\n",
                      lb[0], lb[1], lb[2], lb[3], (unsigned long)avail);
    }
    if (avail == 0) { gpsI2cFails = 0; return 0; }   // clean cycle, no data
    // Cap to a sane chunk so we don't fight Wire's internal buffer.
    // 512 covers ~3 full GGA+RMC+VTG bursts; anything larger we'll get
    // on the next poll.
    if (avail > 512) avail = 512;

    // 3. Data fetch command to 0x50: prefix + uint32 length, little-endian.
    uint8_t fetch[8] = {
        0x08, 0x00, 0x20, 0x51, 0xaa,
        (uint8_t)(avail & 0xff),
        (uint8_t)((avail >> 8) & 0xff),
        (uint8_t)((avail >> 16) & 0xff),
    };
    // (avail < 65536 always here — high byte stays 0.)
    Wire.beginTransmission(kLc76gCmdAddr);
    Wire.write(fetch, sizeof(fetch));
    if (Wire.endTransmission() != 0) { gpsUnstick(); gpsI2cFails++; return -1; }

    // 4. Read avail bytes from 0x54 reg 0x00. Chunked because Wire's
    // internal buffer defaults to 128 — we bump it in initGps() but stay
    // defensive by reading in passes of <= 128.
    int total = 0;
    while (avail > 0) {
        const uint8_t chunk = (avail > 128) ? 128 : (uint8_t)avail;
        // Same bare-requestFrom pattern as step 2 — 0x54 is a stream, not
        // a register-mapped device, so no register-pointer prefix.
        got = Wire.requestFrom(kLc76gReadAddr, chunk);
        if (got == 0) { gpsUnstick(); return total; }
        for (uint8_t i = 0; i < got; i++) {
            gpsFeedByte((uint8_t)Wire.read(), now);
        }
        total += got;
        avail -= got;
        if (got < chunk) break;     // chip returned less than requested
    }
    gps.i2cBytes += (uint32_t)total;
    // We made it through a complete cycle — reset the consecutive-fail
    // counter so a single recovery re-enables the I²C path. (Bytes==0 is
    // still a clean cycle: chip just had nothing buffered for us.)
    gpsI2cFails = 0;
    return total;
}

// Drain any pending response left mid-cycle on 0x54 / 0x58. The LC76G's
// I²C state machine expects strict request -> response alternation: if a
// previous write was issued without the matching read (e.g. across a reset
// boundary, or by an aborted transaction), the chip will NACK further
// writes to 0x50 until we consume the bytes it has queued. Reading 1 byte
// from each of the two response addresses brings it back to "ready". Per
// the Quectel forum thread #27337.
static void gpsUnstick() {
    Wire.requestFrom((uint8_t)0x54, (uint8_t)1);
    while (Wire.available()) (void)Wire.read();
    Wire.requestFrom((uint8_t)0x58, (uint8_t)1);
    while (Wire.available()) (void)Wire.read();
}

// Drain whatever Serial1 has buffered into the parser. Cheap (single
// memory read per byte); safe to call every loop iteration. No-op when
// the chip's TXD1 jumper (R15) isn't populated — the UART line just
// floats and Serial1.available() stays at 0.
static void gpsUartDrain(uint32_t now) {
    while (Serial1.available() > 0) {
        const uint8_t b = (uint8_t)Serial1.read();
        gps.uartBytes++;
        gpsFeedByte(b, now);
    }
}

static bool initGps() {
    Serial.println("Initialising LC76G GNSS...");
    // The chip needs at most 256 bytes per Wire transaction; default 128
    // works but tight. Bump to 512 to give NMEA bursts headroom.
    Wire.setBufferSize(512);

    // Bring up Serial1 on the GPS UART pins. Chip default is 9600 8N1.
    // If R15/R16 0R jumpers are unpopulated on this board variant this
    // does nothing useful, but it costs us nothing either.
    Serial1.begin(kGpsUartBaud, SERIAL_8N1, kGpsUartRx, kGpsUartTx);
    Serial.printf("  UART: Serial1 @ %lu baud (RX=GPIO%d, TX=GPIO%d)\r\n",
                  (unsigned long)kGpsUartBaud, kGpsUartRx, kGpsUartTx);

    // Pulse RESET (active-low) via the TCA9554 expander.
    if (!tcaPinWrite(kLc76gResetExio, false)) {
        Serial.println("  ERROR: TCA9554 RESET-low write failed");
        return false;
    }
    delay(15);
    if (!tcaPinWrite(kLc76gResetExio, true)) {
        Serial.println("  ERROR: TCA9554 RESET-high write failed");
        return false;
    }
    // Quectel datasheet: ~500 ms boot before the host can talk to it.
    delay(600);

    // Drain any stale response state from before our reset (in case a
    // previous boot left the chip's state machine mid-cycle on 0x54/0x58).
    gpsUnstick();

    // No probe write here. The boot-time I²C scan already confirmed 0x50
    // ACKs; doing another write here without consuming its response would
    // leave the chip in "response-pending" state and cause the very next
    // gpsPoll() to fail with NACK. The first poll (or UART drain) will
    // confirm whether the chip is actually streaming.
    Serial.println("  LC76G reset; first poll will confirm streaming.");
    return true;
}

// ---- BLE peripheral -------------------------------------------------------
//
// Layout: one custom 128-bit service (BOAT_BLE_SERVICE_UUID) advertising
// under the name "esp32-boat-tx". Six characteristics: five NOTIFY-only
// telemetry channels and one WRITE-only command channel.
//
// We keep all NimBLE handles as file-scope globals because NimBLE itself
// owns long-lived FreeRTOS tasks that reference them — destroying them
// halfway through a callback would crash. The library never garbage-
// collects them; we never destroy them either.

static NimBLEServer         *bleServer        = nullptr;
static NimBLEService        *bleService       = nullptr;
static NimBLECharacteristic *charWind         = nullptr;
static NimBLECharacteristic *charGps          = nullptr;
static NimBLECharacteristic *charHeading      = nullptr;
static NimBLECharacteristic *charDepthTemp    = nullptr;
static NimBLECharacteristic *charAttitude     = nullptr;
static NimBLECharacteristic *charCommand      = nullptr;

// Stats reset each serial-heartbeat tick so we can see real publish rates.
// (bleNotifyTotal, the per-char notifyCount* counters, and simChannelMask
//  are declared near the top of the file — they're referenced by the
//  multi-page UI in updatePages() which sits above this section.)
static volatile uint32_t bleNotifyCount = 0;

// Server callbacks: we use them for two things — visibility ("[BLE] central
// connected", logged with the peer's MAC so future debugging is easier),
// and to restart advertising as soon as a central disconnects so the next
// reconnect is immediate. Without that, advertising stops once the first
// central connects and would never come back.
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
        char peer[18];
        snprintf(peer, sizeof(peer), "%02x:%02x:%02x:%02x:%02x:%02x",
                 desc->peer_id_addr.val[5], desc->peer_id_addr.val[4],
                 desc->peer_id_addr.val[3], desc->peer_id_addr.val[2],
                 desc->peer_id_addr.val[1], desc->peer_id_addr.val[0]);
        Serial.printf("[BLE] central connected: %s\r\n", peer);
    }
    void onDisconnect(NimBLEServer *server) override {
        Serial.println("[BLE] central disconnected — re-advertising");
        NimBLEDevice::startAdvertising();
    }
};

// Command callback: step 3 just logs the bytes. Step 6 will look at byte 0
// as boatble::CommandType and dispatch on the payload.
class CommandCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        Serial.printf("[BLE] command write (%u bytes):", (unsigned)v.size());
        for (size_t i = 0; i < v.size() && i < 32; i++) {
            Serial.printf(" %02x", (uint8_t)v[i]);
        }
        Serial.println();
    }
};

static bool initBle() {
    Serial.println("Initialising BLE peripheral (NimBLE)...");
    NimBLEDevice::init(BOAT_BLE_DEVICE_NAME);
    // ESP32-S3 BLE TX power: default is +3 dBm. For boat-scale ranges
    // (TX in wiring locker, RX in cockpit, ~5-10 m) the default is fine
    // and conserves power; max (+9 dBm) would be belt-and-suspenders.
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);

    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    bleService = bleServer->createService(BOAT_BLE_SERVICE_UUID);

    charWind = bleService->createCharacteristic(
        BOAT_BLE_WIND_UUID, NIMBLE_PROPERTY::NOTIFY);
    charGps = bleService->createCharacteristic(
        BOAT_BLE_GPS_UUID, NIMBLE_PROPERTY::NOTIFY);
    charHeading = bleService->createCharacteristic(
        BOAT_BLE_HEADING_UUID, NIMBLE_PROPERTY::NOTIFY);
    charDepthTemp = bleService->createCharacteristic(
        BOAT_BLE_DEPTH_TEMP_UUID, NIMBLE_PROPERTY::NOTIFY);
    charAttitude = bleService->createCharacteristic(
        BOAT_BLE_ATTITUDE_UUID, NIMBLE_PROPERTY::NOTIFY);
    charCommand = bleService->createCharacteristic(
        BOAT_BLE_COMMAND_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    charCommand->setCallbacks(new CommandCallbacks());

    bleService->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BOAT_BLE_SERVICE_UUID);
    // Faster reconnect when the RX (or phone) drops and comes back.
    adv->setMinInterval(0x20);   // 0x20 × 0.625 ms = 20 ms
    adv->setMaxInterval(0x40);   // 0x40 × 0.625 ms = 40 ms
    adv->setScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.printf("  Advertising as \"%s\"\r\n", BOAT_BLE_DEVICE_NAME);
    Serial.printf("  Service UUID: %s\r\n", BOAT_BLE_SERVICE_UUID);
    return true;
}

// ---- Simulator ------------------------------------------------------------
//
// Each publishX() builds a PDU with sin / linear values keyed off `t`
// (millis since boot) and pushes it onto its characteristic. Patterns
// are slow enough that an observer in nRF Connect can read them by eye
// while watching the hex view.

// Helper: wrap a fractional value into [0..3600) deg10 space. Useful for
// linearly-rotating directions that should look like a compass spin.
static int16_t wrap_deg10(float deg10) {
    while (deg10 < 0)        deg10 += 3600.0f;
    while (deg10 >= 3600.0f) deg10 -= 3600.0f;
    return (int16_t)deg10;
}

// Helper: increment all three notify-related counters (heartbeat-resetting,
// monotonic total, per-characteristic). Kept inline so the publishers below
// stay short and obviously parallel.
static inline void countNotify(volatile uint32_t &perChar) {
    bleNotifyCount++;
    bleNotifyTotal++;
    perChar++;
}

static void publishWind(uint32_t t) {
    if (!(simChannelMask & boatble::SIM_CH_WIND)) return;
    boatble::WindPdu pdu = {};
    pdu.valid_mask = 0x1F;   // all 5 fields
    pdu.twa_deg10  = (int16_t)(sinf(t / 10000.0f) * 1200);             // ±120°
    pdu.tws_kt100  = (uint16_t)(1500 + sinf(t /  8000.0f) * 500);      // 10-20 kt
    pdu.twd_deg10  = wrap_deg10((float)(t / 100));                     // full rotation / 360 s
    pdu.awa_deg10  = (int16_t)(sinf(t / 12000.0f) *  900);             // ±90°
    pdu.aws_kt100  = (uint16_t)(1200 + cosf(t /  9000.0f) * 400);      // 8-16 kt
    charWind->setValue((uint8_t *)&pdu, sizeof(pdu));
    charWind->notify();
    countNotify(notifyCountWind);
}

static void publishGps(uint32_t t) {
    if (!(simChannelMask & boatble::SIM_CH_GPS)) return;
    boatble::GpsPdu pdu = {};
    pdu.valid_mask = 0x0F;   // all 4 fields
    // Step 8: prefer the real LC76G fix when fresh. COG/SOG are still
    // simulated until we wire RMC parsing — the BLE protocol publishes
    // them together so we always have to fill them.
    if (gpsHasFreshFix(t)) {
        pdu.lat_e7 = (int32_t)llround(gps.lat * 1e7);
        pdu.lon_e7 = (int32_t)llround(gps.lon * 1e7);
    } else {
        pdu.lat_e7 = 561572000;   // Aarhus, DK (sim fallback)
        pdu.lon_e7 = 102107000;
    }
    pdu.cog_deg10  = wrap_deg10((float)(t / 200));                      // full rotation / 720 s
    pdu.sog_kt100  = (uint16_t)(600 + sinf(t / 15000.0f) * 300);        // 3-9 kt
    charGps->setValue((uint8_t *)&pdu, sizeof(pdu));
    charGps->notify();
    countNotify(notifyCountGps);
}

static void publishHeading(uint32_t t) {
    if (!(simChannelMask & boatble::SIM_CH_HEADING)) return;
    boatble::HeadingPdu pdu = {};
    pdu.valid_mask = 0x03;   // HDG + BSPD
    pdu.hdg_deg10  = wrap_deg10((float)((t / 250) + 1800));             // offset from COG
    pdu.bspd_kt100 = (uint16_t)(500 + sinf(t / 12000.0f) * 200);        // 3-7 kt
    charHeading->setValue((uint8_t *)&pdu, sizeof(pdu));
    charHeading->notify();
    countNotify(notifyCountHeading);
}

static void publishDepthTemp(uint32_t t) {
    // We honour the Depth channel mask gate but always include AIR-T and
    // SEA-T fields if they're enabled (they share the same PDU). The
    // mask we send on the wire reflects which fields are currently
    // enabled, so the RX never sees stale 0s for disabled channels.
    if (!(simChannelMask & (boatble::SIM_CH_DEPTH |
                            boatble::SIM_CH_AIR_TEMP))) return;
    boatble::DepthTempPdu pdu = {};
    pdu.valid_mask = 0;
    if (simChannelMask & boatble::SIM_CH_DEPTH) {
        pdu.dep_m10      = (uint16_t)(100 + sinf(t / 30000.0f) * 70);   // 3-17 m
        pdu.sea_temp_c10 = (int16_t)(150 + sinf(t / 60000.0f) * 30);    // 12-18 °C
        pdu.valid_mask  |= (1 << 0) | (1 << 2);                          // DEP + SEA-T
    }
    if (simChannelMask & boatble::SIM_CH_AIR_TEMP) {
        pdu.air_temp_c10 = (int16_t)(180 + sinf(t / 50000.0f) * 30);    // 15-21 °C
        pdu.valid_mask  |= (1 << 1);                                     // AIR-T
    }
    charDepthTemp->setValue((uint8_t *)&pdu, sizeof(pdu));
    charDepthTemp->notify();
    countNotify(notifyCountDepthTemp);
}

static void publishAttitude(uint32_t t) {
    if (!(simChannelMask & boatble::SIM_CH_ATTITUDE)) return;
    boatble::AttitudePdu pdu = {};
    pdu.valid_mask  = 0x01;   // HEEL only; PITCH/ROT/RUD/ENG-T/OIL-T masked invalid
    pdu.heel_deg10  = (int16_t)(sinf(t / 5000.0f) * 100);               // ±10°
    // pitch / rot / rud / eng_temp / oil_temp left at 0 — invalid_mask = 0
    charAttitude->setValue((uint8_t *)&pdu, sizeof(pdu));
    charAttitude->notify();
    countNotify(notifyCountAttitude);
}

// Sweep the full 7-bit address range and report every responder. Each chip
// on this board has a known address, so this output doubles as a hardware
// sanity check — if anything is missing on boot, that's a wiring or
// pull-up problem rather than a software bug.
static void scanFullBus() {
    Serial.println("Full I²C device scan:");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            const char *known = "?";
            switch (addr) {
                case 0x18: known = "ES8311 audio codec"; break;
                case 0x20: known = "TCA9554 GPIO expander"; break;
                case 0x34: known = "AXP2101 PMIC"; break;
                case 0x40: known = "ES7210 mic ADC"; break;
                case 0x50: known = "LC76G GNSS (cmd channel)"; break;
                case 0x51: known = "PCF85063 RTC"; break;
                case 0x5a: known = "CST9217 touch"; break;
                case 0x6a: known = "QMI8658 IMU"; break;
                case 0x6b: known = "QMI8658 IMU (alt addr)"; break;
            }
            Serial.printf("  0x%02x  %s\r\n", addr, known);
            found++;
        }
    }
    Serial.printf("%d device(s) responded.\r\n", found);
}

void setup() {
    Serial.begin(115200);

    // Native USB-CDC race on ESP32-S3 (see step 1b notes): pio monitor
    // doesn't start reading the stream until 2-3 s after USB enumeration,
    // and HWCDC's `operator bool()` flips to true too early to gate on.
    // Block 4 s unconditionally so the boot banner survives. Drops out
    // in step 1c-iv once the AMOLED itself becomes the boot diagnostic.
    delay(4000);

    Serial.println();
    Serial.println("================================================");
    Serial.println("  esp32-boat TX — step 9 (UI polish + tap diag)");
    Serial.println("================================================");
    Serial.printf("  Built: %s %s\r\n", __DATE__, __TIME__);
    Serial.printf("  Chip:  ESP32-S3, %d MHz\r\n", ESP.getCpuFreqMHz());
    Serial.printf("  Flash: %d MB\r\n", ESP.getFlashChipSize() / (1024 * 1024));
    Serial.printf("  PSRAM: %d KB free of %d KB total\r\n",
                  ESP.getFreePsram() / 1024, ESP.getPsramSize() / 1024);
    Serial.println("================================================");
    Serial.println();

    // ---- 1. bring up the I²C bus --------------------------------------------
    Serial.printf("Using I²C bus: SDA=GPIO%d, SCL=GPIO%d\r\n", kI2cSda, kI2cScl);
    if (!Wire.begin(kI2cSda, kI2cScl, 100000)) {
        Serial.println("ERROR: Wire.begin() failed. Halting before any further "
                       "I²C traffic — driver error.");
        return;
    }
    Serial.println();

    // ---- 2. enumerate every chip on the bus --------------------------------
    scanFullBus();
    Serial.println();

    // ---- 3. bring up the AXP2101 driver ------------------------------------
    Serial.println("Initialising XPowersLib (AXP2101)...");
    if (!PMU.begin(Wire, kAxp2101Addr, kI2cSda, kI2cScl)) {
        Serial.println("ERROR: PMU.begin() returned false despite 0x34 being on");
        Serial.println("       the bus. Likely a library/version mismatch.");
        return;
    }
    Serial.printf("  Chip model: %d (4 = AXP2101)\r\n", PMU.getChipModel());
    Serial.printf("  VBUS present:    %s\r\n", PMU.isVbusIn() ? "yes" : "no");
    Serial.printf("  Battery present: %s\r\n", PMU.isBatteryConnect() ? "yes" : "no");
    Serial.printf("  Battery voltage: %u mV\r\n", PMU.getBattVoltage());
    Serial.printf("  Battery %%:       %d %%\r\n", PMU.getBatteryPercent());
    Serial.printf("  Charging:        %s\r\n", PMU.isCharging() ? "yes" : "no");
    Serial.println();

    // ---- 4. bring up the AMOLED panel -------------------------------------
    if (initDisplay()) {
        if (initLvgl()) {
            initPages();
        }
    }
    Serial.println();

    // ---- 5. bring up touch -------------------------------------------------
    touchReady = initTouch();
    Serial.println();

    // ---- 6. bring up GPS ---------------------------------------------------
    initGps();   // soft-fail OK; publishGps() falls back to sim if no fix
    Serial.println();

    // ---- 7. bring up BLE peripheral + simulator ---------------------------
    initBle();
    Serial.println();

    Serial.println("Step 8 done. Swipe left/right to change page:");
    Serial.println("  Primary - Simulator - PGN - Settings - Communication");
    Serial.println("================================================");
    Serial.println();
}

// Non-blocking loop. Five gates compete for our time:
//   - LVGL refresh (tick + timer_handler every iteration)
//   - BLE publish gates (4× 100 ms, 1× 500 ms)
//   - UI page refresh (500 ms)
//   - Touch poll (every iteration — cheap I²C round trip)
//   - Serial heartbeat (5 s)
// All run off the same millis() polling, no FreeRTOS task math needed.
static uint32_t lvLastMs           = 0;
static uint32_t hbLastMs           = 0;
static uint32_t windPubMs          = 0;
static uint32_t gpsPubMs           = 0;
static uint32_t hdgPubMs           = 0;
static uint32_t depthTempPubMs     = 0;
static uint32_t attitudePubMs      = 0;
static uint32_t uiUpdateMs         = 0;
static uint32_t gpsPollMs          = 0;
static uint32_t bleNotifyLast      = 0;
static uint32_t bleNotifyLastT     = 0;
static uint32_t cachedRatePerSec   = 0;

static constexpr uint32_t kHeartbeatPeriodMs = 5000;
static constexpr uint32_t kFastPubPeriodMs   = 100;   // 10 Hz
static constexpr uint32_t kSlowPubPeriodMs   = 500;   // 2 Hz
static constexpr uint32_t kUiUpdatePeriodMs  = 500;   // 2 Hz UI refresh
static constexpr uint32_t kGpsPollPeriodMs   = 200;   // 5 Hz GPS poll (chip emits @ 1 Hz)

void loop() {
    const uint32_t now = millis();

    // LVGL refresh.
    lv_tick_inc(now - lvLastMs);
    lvLastMs = now;
    lv_timer_handler();

    // BLE publish gates. notify() is cheap when no central is subscribed,
    // so we publish unconditionally — much simpler than tracking
    // per-characteristic subscribe state and the wire cost is zero.
    if (now - windPubMs      >= kFastPubPeriodMs) { windPubMs      = now; publishWind(now); }
    if (now - gpsPubMs       >= kFastPubPeriodMs) { gpsPubMs       = now; publishGps(now); }
    if (now - hdgPubMs       >= kFastPubPeriodMs) { hdgPubMs       = now; publishHeading(now); }
    if (now - depthTempPubMs >= kSlowPubPeriodMs) { depthTempPubMs = now; publishDepthTemp(now); }
    if (now - attitudePubMs  >= kFastPubPeriodMs) { attitudePubMs  = now; publishAttitude(now); }

    // UI refresh — compute notify rate over the elapsed window, then
    // hand the current snapshot to updatePages(). Cached so the Primary
    // page reads a steady value rather than recomputing each call.
    if (now - uiUpdateMs >= kUiUpdatePeriodMs) {
        uint32_t dtMs = (bleNotifyLastT == 0) ? 1 : (now - bleNotifyLastT);
        if (dtMs == 0) dtMs = 1;
        cachedRatePerSec  = ((bleNotifyTotal - bleNotifyLast) * 1000) / dtMs;
        bleNotifyLast     = bleNotifyTotal;
        bleNotifyLastT    = now;
        uiUpdateMs        = now;
        uint32_t connected = bleServer ? bleServer->getConnectedCount() : 0;
        updatePages(now, connected, cachedRatePerSec);
    }

    // GPS UART drain — every iteration. Free when Serial1 is empty (one
    // memory-mapped register read). When R15 is populated on this board,
    // this is the primary data path; when not, it's a no-op.
    gpsUartDrain(now);

    // GPS I²C poll. Chip emits at ~1 Hz; polling at 5 Hz catches each new
    // sentence batch within ≤200 ms. gpsPoll() is a no-op when the chip
    // has no buffered bytes (length-query returns 0). Currently returning
    // i2cRead -1 on this board variant; left running so we know if a
    // future configuration change wakes it up.
    if (now - gpsPollMs >= kGpsPollPeriodMs) {
        gpsPollMs = now;
        gpsPoll(now);
    }

    // Touch-driven page nav. Polled every iteration (~200 Hz given the
    // trailing delay(5)) so swipes feel responsive. getTouchPoints() is
    // one short I²C round trip when no finger is down, so cost is low.
    SwipeDir swipe = pollTouch(now);
    if (swipe == SWIPE_LEFT || swipe == SWIPE_RIGHT) {
        TxPage target = (swipe == SWIPE_LEFT)
            ? (TxPage)((currentPage + 1) % TXP_COUNT)
            : (TxPage)((currentPage + TXP_COUNT - 1) % TXP_COUNT);
        showPage(target);
        // Force an immediate UI refresh so the new page isn't blank
        // for up to kUiUpdatePeriodMs.
        uint32_t connected = bleServer ? bleServer->getConnectedCount() : 0;
        updatePages(now, connected, cachedRatePerSec);
        Serial.printf("[UI] swipe-%s -> %s\r\n",
                      swipe == SWIPE_LEFT ? "left" : "right",
                      kPageTitles[currentPage]);
    }

    // Serial heartbeat. Reports power state from the AXP2101 plus BLE
    // state (central count + notifies emitted since the last heartbeat).
    if (now - hbLastMs >= kHeartbeatPeriodMs) {
        const uint32_t notifies = bleNotifyCount;
        bleNotifyCount = 0;
        const uint32_t connected = bleServer ? bleServer->getConnectedCount() : 0;
        hbLastMs = now;
        Serial.printf("[TX] uptime=%lu ms  vbat=%u mV  vbus=%s  charging=%s "
                      "ble=%u central(s)  notifies=%lu in %lu ms  page=%s\r\n",
                      now,
                      PMU.getBattVoltage(),
                      PMU.isVbusIn() ? "yes" : "no",
                      PMU.isCharging() ? "yes" : "no",
                      (unsigned)connected,
                      (unsigned long)notifies,
                      (unsigned long)kHeartbeatPeriodMs,
                      kPageTitles[currentPage]);
        // Step 8 — GPS diagnostic. Two byte counters so we can see which
        // physical path (I²C or UART) is actually feeding the parser on
        // this board variant. linesParsed climbs only when a complete
        // sentence with valid checksum is received.
        Serial.printf("[gps] i2c=%lu uart=%lu lines=%lu fix=%u sats=%u last=\"%s\"\r\n",
                      (unsigned long)gps.i2cBytes,
                      (unsigned long)gps.uartBytes,
                      (unsigned long)gps.linesParsed,
                      (unsigned)gps.fixQuality,
                      (unsigned)gps.numSats,
                      gps.lastLine[0] ? gps.lastLine : "(none)");
    }

    // Small yield to the scheduler so NimBLE's host task and the FreeRTOS
    // idle hook can run. 5 ms is the standard LVGL polling interval.
    delay(5);
}
