// AIS-radar device — milestone 0: salvage bring-up (ADR-0016).
//
// Proves the two salvaged subsystems work together on the new per-device
// tree: the AMOLED display (shared/display, LVGL over Arduino_GFX) shows the
// target count decoded by the salvaged AIVDM decoder (shared/ais) from
// verified test sentences.
//
// Next steps (docs/ROADMAP.md "v2 — Device 1"): LC76G GPS + Daisy AIS on a
// UART (live targets), the LVGL radar render, and the BLE link to iOS.

#include <Arduino.h>
#include <lvgl.h>
#include <AmoledDisplay.h>
#include <Touch.h>
#include <Lc76gGps.h>
#include <AisTargetDecoder.h>
#include <default_sentence_parser.h>

static AisTargetDecoder decoder;
static AIS::DefaultSentenceParser parser;

// Feed one raw AIVDM sentence; decodeMsg must be called until it returns 0.
// Multi-fragment messages reassemble across successive feed() calls.
static void feed(const char* sentence) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", sentence);
    size_t i = 0;
    do { i = decoder.decodeMsg(buf, (size_t)n, i, parser); } while (i != 0);
}

static lv_obj_t* g_label = nullptr;
static int16_t g_tapX = -1, g_tapY = -1;

static void refreshSummary() {
    if (!g_label) return;
    const gps::Fix& f = gps::fix();
    char gpsLine[48];
    if (f.fixQuality >= 1)
        snprintf(gpsLine, sizeof(gpsLine), "GPS %.4f, %.4f", f.lat, f.lon);
    else
        snprintf(gpsLine, sizeof(gpsLine), "GPS no fix (%lu B)",
                 (unsigned long)f.uartBytes);
    char tapLine[32];
    if (g_tapX >= 0) snprintf(tapLine, sizeof(tapLine), "tap %d,%d", g_tapX, g_tapY);
    else             snprintf(tapLine, sizeof(tapLine), "tap —");
    lv_label_set_text_fmt(g_label, "AIS-radar\n%u targets\n%s\n%s",
                          (unsigned)decoder.store().size(), gpsLine, tapLine);
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[ais-radar] milestone 0 — AMOLED + AIS decode salvage (ADR-0016)");

    const bool haveDisplay = amoled::begin();
    touch::begin();
    gps::begin();   // UART-only; silent until R15/R16 jumpers are soldered

    // Verified sentences (correct checksums) from the AIS test-sentence set.
    feed("!AIVDM,1,1,,B,B52K>7008h>KUH7v8L0L;wv00000,0*59");  // type 18  Class B position
    feed("!AIVDM,1,1,,A,15Memvh01E0qcJ0Op:D:ggwp00000,5*2C");  // type 1   Class A position
    feed("!AIVDM,2,1,1,A,55Memvh2;HNMMA@h001@U@4r0<58Lt0000000016000000000Bhkl1CR0AiC,0*53"); // type 5 (1/2)
    feed("!AIVDM,2,2,1,A,P0000000000,2*45");                   // type 5   (2/2) name + type

    Serial.printf("[ais] %u target(s) from %llu message(s)\n",
                  (unsigned)decoder.store().size(),
                  (unsigned long long)decoder.messages());

    if (haveDisplay) {
        g_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_align(g_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(g_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(g_label);
        refreshSummary();
    } else {
        Serial.println("[ais-radar] display unavailable — serial only");
    }
}

void loop() {
    static uint32_t last = 0;
    static uint32_t lastRefresh = 0;
    const uint32_t now = millis();
    gps::poll(now);
    int16_t tx, ty;
    if (touch::readPoint(tx, ty)) { g_tapX = tx; g_tapY = ty; }
    lv_tick_inc(now - last);
    last = now;
    if (now - lastRefresh >= 500) { lastRefresh = now; refreshSummary(); }
    lv_timer_handler();
    delay(5);
}
