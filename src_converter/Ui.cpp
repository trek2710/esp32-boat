#include "Ui.h"
#include "AisTargetStore.h"
#include "WifiPublisher.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace ui {

// Pin map matches AndroidCrypto/ESP32_C6_Waveshare_ST7789_Starter for the
// Waveshare ESP32-C6-LCD-1.47.
static constexpr int8_t kPinDC   = 15;
static constexpr int8_t kPinCS   = 14;
static constexpr int8_t kPinSCK  = 7;
static constexpr int8_t kPinMOSI = 6;
static constexpr int8_t kPinRST  = 21;
static constexpr int8_t kPinBL   = 22;

// Landscape rotation 1. The 240x320 ST7789 controller crops to 172x320,
// 34-px offset on the short axis. Arduino_TFT::setRotation case 1 sets
// `_yStart = COL_OFFSET2`, so the 34 must live in col_offset2.
static constexpr int16_t kScreenW = 320;
static constexpr int16_t kScreenH = 172;
static constexpr int16_t kColOff1 = 34;
static constexpr int16_t kRowOff1 = 0;
static constexpr int16_t kColOff2 = 34;
static constexpr int16_t kRowOff2 = 0;

// Layout.
static constexpr int16_t kHeaderH       = 38;
static constexpr int16_t kColHdrY       = 44;
static constexpr int16_t kListTop       = 70;
static constexpr int16_t kListRowH      = 18;
static constexpr int16_t kListRows      = 4;
static constexpr int16_t kFooterY       = 156;

static constexpr uint32_t kBoatFlashMs  = 400;
static constexpr uint32_t kDaisyStaleMs = 30000;

// Round 85 (v1.6 step 1): AIS filter values come from the live settings
// snapshot (ADR-0013, `ais.hide_anchored` / `ais.stale_s`). `range_nm` is
// unused on this peer — the converter's target store has no lat/lon so
// distance can't be computed here; the filter is meaningful on the LCDs.

// Backlight PWM + idle-dim. analogWrite() on Arduino-ESP32 3.x uses LEDC at
// 1 kHz, no flicker. 8-bit duty: 102 = 40 %, 13 = ~5 %.
static constexpr int8_t   kPinBootBtn        = 9;
static constexpr uint8_t  kBacklightActive   = 102;
static constexpr uint8_t  kBacklightIdle     = 13;
static constexpr uint32_t kIdleTimeoutMs     = 5UL * 60UL * 1000UL;  // 5 min

// RGB565 palette tuned for a white background.
static constexpr uint16_t kBg        = 0xFFFF;   // white page
static constexpr uint16_t kFg        = 0x0000;   // body text, black
static constexpr uint16_t kDim       = 0x8C71;   // mid-gray
static constexpr uint16_t kDark      = 0x4208;   // dark gray
static constexpr uint16_t kAccent    = 0x0011;   // deep blue accent
static constexpr uint16_t kAmber     = 0xC300;   // dark orange/amber
static constexpr uint16_t kAmberHot  = 0xFB00;   // bright orange (boat flash)
static constexpr uint16_t kGreen     = 0x0320;   // dark green
static constexpr uint16_t kGreenLite = 0x07E0;   // bright green (Daisy badge)
static constexpr uint16_t kForest    = 0x0560;   // forest green (sailing)
static constexpr uint16_t kRed       = 0xC000;   // dark red
static constexpr uint16_t kBlue      = 0x0019;   // saturated dark blue
static constexpr uint16_t kMagenta   = 0x9013;
static constexpr uint16_t kOlive     = 0x6260;

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX*     gfx = nullptr;

// Memoised state.
static int      last_src          = -2;
static uint32_t last_n2k_sent     = UINT32_MAX;
static uint32_t last_raw_seen     = UINT32_MAX;
static uint32_t last_uptime_s     = UINT32_MAX;
static bool     last_daisy        = false;
static uint16_t last_boat_fill    = 0xFFFF;  // sentinel — force first draw
static size_t   last_target_count = SIZE_MAX;
static char     last_wifi_line[32] = {0};   // memoised wifi-status footer line

static uint32_t last_activity_ms  = 0;
static uint32_t prev_tx_ms_seen   = 0;
static uint8_t  current_backlight = 0;

static uint16_t navStatusColor(uint8_t nav) {
    switch (nav) {
        case 0:  return kGreen;
        case 1:  return kAmber;
        case 2:  return kRed;
        case 3:  return kRed;
        case 4:  return kAmber;
        case 5:  return kAmber;
        case 6:  return kRed;
        case 7:  return kBlue;
        case 8:  return kForest;
        case 14: return kRed;
        default: return kFg;
    }
}

static const char* typeAbbrev(uint8_t t) {
    if (t == 0) return "   ";
    if (t >= 20 && t <= 29) return "WIG";
    if (t == 36)            return "SAL";
    if (t == 37)            return "PLS";
    if (t >= 30 && t <= 39) return "FSH";
    if (t >= 40 && t <= 49) return "HSC";
    if (t == 50)            return "PLT";
    if (t == 51)            return "SAR";
    if (t == 52)            return "TUG";
    if (t == 53)            return "TND";
    if (t == 54)            return "POL";
    if (t == 55)            return "LAW";
    if (t == 58)            return "MED";
    if (t >= 60 && t <= 69) return "PAX";
    if (t >= 70 && t <= 79) return "CRG";
    if (t >= 80 && t <= 89) return "TNK";
    return "OTH";
}

static uint16_t typeColor(uint8_t t) {
    if (t >= 80 && t <= 89) return kRed;
    if (t >= 70 && t <= 79) return kAmber;
    if (t >= 60 && t <= 69) return kBlue;
    if (t == 36 || t == 37) return kForest;
    if (t >= 30 && t <= 39) return kMagenta;
    if (t == 50 || t == 51 || t == 52) return kOlive;
    return kDim;
}

static void drawDaisyBadge(bool fresh) {
    constexpr int16_t x = 178, y = 6, w = 70, h = 22;
    const uint16_t fill   = fresh ? kGreenLite : kRed;
    const uint16_t border = fill;
    const uint16_t text   = fresh ? kFg : kBg;  // black on lime, white on red
    gfx->fillRoundRect(x, y, w, h, 5, fill);
    gfx->drawRoundRect(x, y, w, h, 5, border);
    gfx->setTextSize(1);
    // Fake-bold by drawing twice with a 1px horizontal offset. Arduino_GFX
    // default 5x7 font has no bold weight; this thickens each glyph stroke.
    gfx->setTextColor(text, fill);
    gfx->setCursor(x + 10, y + 8);
    gfx->print("DAISY");
    gfx->setTextColor(text);  // transparent bg so we don't erase the first pass
    gfx->setCursor(x + 11, y + 8);
    gfx->print("DAISY");
}

// Bigger boat ~48x36 pixel sailboat icon: filled sail triangle, mast,
// trapezoid hull. Drawn with a black outline so it's always visible on
// white; the fill colour conveys idle vs. flash state.
static void drawBoat(int16_t x, int16_t y, uint16_t fill) {
    constexpr int16_t w = 48, h = 36;
    // Sail (filled triangle, point at top-right of hull, base on hull).
    const int16_t sx1 = x + 22,         sy1 = y + 0;
    const int16_t sx2 = x + 22,         sy2 = y + h - 12;
    const int16_t sx3 = x + w - 8,      sy3 = y + h - 12;
    gfx->fillTriangle(sx1, sy1, sx2, sy2, sx3, sy3, fill);
    gfx->drawTriangle(sx1, sy1, sx2, sy2, sx3, sy3, kFg);
    // Mast.
    gfx->drawFastVLine(x + 22, y + 0, h - 12, kFg);
    // Hull: trapezoid as filled rect + two right triangles.
    const int16_t hy = y + h - 10;
    gfx->fillTriangle(x + 0,     hy,      x + 8,     hy + 8, x + 8,     hy,     fill);
    gfx->drawLine    (x + 0,     hy,      x + 8,     hy + 8,                     kFg);
    gfx->drawLine    (x + 8,     hy + 8,  x + 8,     hy,                         kFg);
    gfx->fillRect    (x + 8,     hy,      w - 16, 8, fill);
    gfx->drawFastHLine(x + 8,    hy,      w - 16,    kFg);
    gfx->drawFastHLine(x + 8,    hy + 7,  w - 16,    kFg);
    gfx->fillTriangle(x + w - 8, hy,      x + w - 8, hy + 8, x + w,     hy,     fill);
    gfx->drawLine    (x + w - 8, hy,      x + w,     hy,                         kFg);
    gfx->drawLine    (x + w - 8, hy,      x + w - 8, hy + 8,                     kFg);
    gfx->drawLine    (x + w,     hy,      x + w - 8, hy + 8,                     kFg);
}

static void drawBoatArea(uint16_t fill) {
    constexpr int16_t bx = 264, by = 2;
    gfx->fillRect(bx, by, 52, 38, kBg);
    drawBoat(bx, by, fill);
}

void begin() {
    bus = new Arduino_ESP32SPI(kPinDC, kPinCS, kPinSCK, kPinMOSI, GFX_NOT_DEFINED);
    gfx = new Arduino_ST7789(bus, kPinRST, /*rotation=*/1, /*ips=*/true,
                             /*native w=*/172, /*native h=*/320,
                             kColOff1, kRowOff1, kColOff2, kRowOff2);
    gfx->begin();
    gfx->fillScreen(kBg);

    pinMode(kPinBL, OUTPUT);
    analogWrite(kPinBL, kBacklightActive);
    current_backlight = kBacklightActive;
    pinMode(kPinBootBtn, INPUT_PULLUP);   // BOOT button is the only manual
                                          // wake we have on the non-touch SKU
    last_activity_ms = millis();
    gfx->setTextWrap(false);

    // Title.
    gfx->setTextSize(2);
    gfx->setTextColor(kFg, kBg);
    gfx->setCursor(4, 6);
    gfx->print("AIS BRIDGE");

    // Separator line under header.
    gfx->drawFastHLine(0, kHeaderH, kScreenW, kAccent);

    // Column header — labels positioned to align with row data below.
    gfx->setTextSize(2);
    gfx->setTextColor(kAccent, kBg);
    gfx->setCursor(4, kColHdrY);
    gfx->print("Typ Kts  Deg  Name");

    drawDaisyBadge(false);
    drawBoatArea(kDim);
}

static void formatUptime(char* out, size_t cap, uint32_t s) {
    const uint32_t h = s / 3600;
    const uint32_t m = (s % 3600) / 60;
    const uint32_t r = s % 60;
    if (h)      snprintf(out, cap, "%luh%02lum", (unsigned long)h, (unsigned long)m);
    else if (m) snprintf(out, cap, "%lum%02lus", (unsigned long)m, (unsigned long)r);
    else        snprintf(out, cap, "%lus", (unsigned long)r);
}

void refresh(const AisTargetStore& store, const Stats& stats) {
    if (!gfx) return;
    const uint32_t now = millis();

    // Activity tracking — any decoded AIS frame or BOOT button press
    // counts. Drives the backlight idle-dim below.
    if (stats.last_tx_ms != prev_tx_ms_seen) {
        last_activity_ms = now;
        prev_tx_ms_seen = stats.last_tx_ms;
    }
    if (digitalRead(kPinBootBtn) == LOW) {
        last_activity_ms = now;
    }
    // Round 85 v1.6 step 1 (ADR-0013): active backlight follows
    // ui.brightness (0..100). Idle dim drops to 5 % (kBacklightIdle).
    // Idle timeout still hardcoded at kIdleTimeoutMs for now; tying it
    // to ui.idle_dim_after_s is the next polish pass.
    const settings::Settings& cfg_bl = WifiPublisher::currentSettings();
    const uint16_t active_pwm = (uint16_t)cfg_bl.ui_brightness * 255 / 100;
    const uint8_t want_bl = (now - last_activity_ms > kIdleTimeoutMs)
                                ? kBacklightIdle
                                : (uint8_t)active_pwm;
    if (want_bl != current_backlight) {
        analogWrite(kPinBL, want_bl);
        current_backlight = want_bl;
    }

    // Indicators. Daisy badge is green only while a sentence has been seen
    // within the last kDaisyStaleMs window — otherwise red. This lets a
    // disconnected or silent receiver show up at a glance.
    const bool daisy = (stats.last_rx_ms != 0) &&
                       (now - stats.last_rx_ms < kDaisyStaleMs);
    if (daisy != last_daisy) {
        drawDaisyBadge(daisy);
        last_daisy = daisy;
    }
    // Two-tier boat state. Base colour = WiFi virtual-bus connectivity
    // (forest green when at least one other peer is visible — i.e. the
    // converter is talking to the bus; dim grey when alone). Brief amber
    // flash overrides the base whenever an N2K message is transmitted.
    const bool n2k_flash = (stats.last_tx_ms != 0) &&
                           (now - stats.last_tx_ms < kBoatFlashMs);
    const bool wifi_visible = (WifiPublisher::peerCount() > 0) ||
                              (WifiPublisher::stationCount() > 0);
    const uint16_t boat_fill =
        n2k_flash    ? kAmberHot :
        wifi_visible ? kForest   : kDim;
    if (boat_fill != last_boat_fill) {
        drawBoatArea(boat_fill);
        last_boat_fill = boat_fill;
    }

    // Target rows. Layout (24 chars at size 2):
    //   0..2   type (type colour)
    //   3      space
    //   4..7   speed, right-aligned 4 chars (e.g. "12.3", " 5.1")
    //   8      space
    //   9..11  course, right-aligned 3 chars (e.g. "287")
    //   12..13 two spaces
    //   14..23 name (or MMSI tail), left-aligned 10 chars
    // Round 85: pull a larger snapshot so filters can reject without
    // starving the visible list. Apply ais.hide_anchored + ais.stale_s
    // (range_nm is N/A on the converter — no lat/lon on the store).
    AisTarget raw[AisTargetStore::CAPACITY];
    const size_t raw_n = store.snapshotByRecency(raw, AisTargetStore::CAPACITY);
    const settings::Settings& cfg = WifiPublisher::currentSettings();
    const uint32_t stale_ms = (uint32_t)cfg.ais_stale_s * 1000UL;
    AisTarget snapshot[kListRows];
    size_t n = 0;
    const uint32_t now_ms = millis();
    for (size_t i = 0; i < raw_n && n < kListRows; ++i) {
        const AisTarget& cand = raw[i];
        if (cfg.ais_hide_anchored &&
            (cand.nav_status == 1 ||   // at anchor
             cand.nav_status == 5 ||   // moored
             cand.nav_status == 6)) {  // aground
            continue;
        }
        if (now_ms - cand.last_seen_ms > stale_ms) continue;
        snapshot[n++] = cand;
    }
    gfx->setTextSize(2);
    for (size_t i = 0; i < kListRows; ++i) {
        const int16_t y = kListTop + static_cast<int16_t>(i) * kListRowH;
        if (i >= n) {
            gfx->fillRect(0, y, kScreenW, kListRowH, kBg);
            continue;
        }
        const AisTarget& t = snapshot[i];

        // Whole-row colour bar: Class A uses nav status, Class B uses ship
        // type. White text on top — colours below are saturated enough to
        // give >4.5:1 contrast for the row text.
        const uint16_t row_bg = (t.klass == 'A')
            ? navStatusColor(t.nav_status)
            : typeColor(t.vessel_type);
        gfx->fillRect(0, y, kScreenW, kListRowH, row_bg);

        const char* typ = typeAbbrev(t.vessel_type);

        char sog_s[6];
        if (t.sog_kn < 0)         strcpy(sog_s, "  --");
        else if (t.sog_kn >= 99.0f) strcpy(sog_s, " 99+");
        else                       snprintf(sog_s, sizeof(sog_s), "%4.1f", t.sog_kn);

        char cog_s[6];
        if (t.cog_deg < 0) strcpy(cog_s, "---");
        else               snprintf(cog_s, sizeof(cog_s), "%3.0f", t.cog_deg);

        char ident[12];
        if (t.name[0] && t.name[0] != ' ') {
            strncpy(ident, t.name, 10);
            ident[10] = 0;
        } else {
            snprintf(ident, sizeof(ident), "%lu", (unsigned long)(t.mmsi % 100000000UL));
        }

        gfx->setCursor(4, y + 1);
        gfx->setTextColor(kBg, row_bg);
        char line[28];
        snprintf(line, sizeof(line), "%-3s %4s %3s  %-10.10s", typ, sog_s, cog_s, ident);
        gfx->print(line);
    }

    // WiFi role line (between target list and footer). Compact format:
    //   "wifi:AP (2 sta)"         — when this converter is the AP
    //   "wifi:STA / esp32-boat-rx" — when STA, with current AP peer
    //   "wifi:electing"            — during cold-boot election
    char wifi_line[32];
    const char* role = WifiPublisher::roleName();
    if (strcmp(role, "AP") == 0) {
        snprintf(wifi_line, sizeof(wifi_line), "wifi:AP (%u sta)",
                 (unsigned)WifiPublisher::stationCount());
    } else if (strcmp(role, "STA") == 0) {
        const char* ap = WifiPublisher::currentApPeer();
        snprintf(wifi_line, sizeof(wifi_line), "wifi:STA / %s",
                 (ap && *ap) ? ap : "(none)");
    } else {
        snprintf(wifi_line, sizeof(wifi_line), "wifi:%s", role);
    }
    if (strcmp(wifi_line, last_wifi_line) != 0) {
        gfx->fillRect(0, 144, kScreenW, 11, kBg);
        gfx->setTextSize(1);
        gfx->setTextColor(kDark, kBg);
        gfx->setCursor(4, 145);
        gfx->print(wifi_line);
        strncpy(last_wifi_line, wifi_line, sizeof(last_wifi_line) - 1);
        last_wifi_line[sizeof(last_wifi_line) - 1] = 0;
    }

    // Footer.
    const size_t total = store.size();
    const uint32_t up_s = now / 1000;
    if (total != last_target_count ||
        stats.n2k_sent != last_n2k_sent ||
        stats.nmea0183_sentences_seen != last_raw_seen ||
        up_s != last_uptime_s ||
        stats.n2k_src_addr != last_src) {
        gfx->fillRect(0, kFooterY, kScreenW, 14, kBg);
        gfx->setTextSize(1);

        gfx->setTextColor(kDim, kBg);
        gfx->setCursor(4, kFooterY + 3);
        char up[16];
        formatUptime(up, sizeof(up), up_s);
        char left[40];
        snprintf(left, sizeof(left), "%u tgts  up:%s  src:%d",
                 static_cast<unsigned>(total), up, stats.n2k_src_addr);
        gfx->print(left);

        char right[40];
        snprintf(right, sizeof(right), "n2k:%lu-0183:%lu",
                 (unsigned long)stats.n2k_sent,
                 (unsigned long)stats.nmea0183_sentences_seen);
        const int16_t right_w = static_cast<int16_t>(strlen(right)) * 6;
        gfx->setCursor(kScreenW - right_w - 12, kFooterY + 3);
        gfx->setTextColor(kAccent, kBg);
        gfx->print(right);

        last_target_count = total;
        last_n2k_sent = stats.n2k_sent;
        last_raw_seen = stats.nmea0183_sentences_seen;
        last_uptime_s = up_s;
        last_src = stats.n2k_src_addr;
    }
}

}  // namespace ui
