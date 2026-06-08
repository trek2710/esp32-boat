// AIS-radar device (ADR-0016) — display + touch + GPS + live Daisy AIS.
//
// Reads the Wegmatt dAISy 2+ on IO16 (Serial2 @ 38400, see
// docs/hardware/ais_radar_wiring), decodes AIVDM into the target store, and
// shows counts on the AMOLED. Next: the LVGL radar render + BLE link to iOS.

#include <Arduino.h>
#include <lvgl.h>
#include <AmoledDisplay.h>
#include <Touch.h>
#include <Lc76gGps.h>
#include <AisTargetDecoder.h>
#include <default_sentence_parser.h>

static constexpr uint32_t kDaisyBaud = 38400;   // dAISy "NMEA HS" default

static AisTargetDecoder decoder;
static AIS::DefaultSentenceParser parser;

static void feed(const char* sentence) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", sentence);
    size_t i = 0;
    do { i = decoder.decodeMsg(buf, (size_t)n, i, parser); } while (i != 0);
}

// ---- Daisy 2+ AIS on IO16 (Serial2 @ 38400) -------------------------------
static uint32_t g_daisyBytes = 0;
static uint32_t g_daisyLines = 0;
static char     g_daisyLine[120];
static int      g_daisyLen = 0;
static char     g_lastLine[120] = {0};

static void daisyPoll() {
    while (Serial2.available() > 0) {
        char c = (char)Serial2.read();
        g_daisyBytes++;
        if (c == '\n') {
            if (g_daisyLen > 0) {
                g_daisyLine[g_daisyLen] = '\0';
                g_daisyLines++;
                strncpy(g_lastLine, g_daisyLine, sizeof(g_lastLine) - 1);
                feed(g_daisyLine);
            }
            g_daisyLen = 0;
        } else if (c != '\r' && g_daisyLen < (int)sizeof(g_daisyLine) - 1) {
            g_daisyLine[g_daisyLen++] = c;
        }
    }
}

static lv_obj_t* g_label = nullptr;

static void refreshSummary() {
    if (!g_label) return;
    const gps::Fix& f = gps::fix();
    char gpsLine[48];
    if (f.fixQuality >= 1)
        snprintf(gpsLine, sizeof(gpsLine), "GPS %.4f, %.4f", f.lat, f.lon);
    else
        snprintf(gpsLine, sizeof(gpsLine), "GPS no fix (%lu B)",
                 (unsigned long)f.uartBytes);
    lv_label_set_text_fmt(g_label, "AIS-radar\n%u targets\nAIS %lu B / %lu ln\n%s",
                          (unsigned)decoder.store().size(),
                          (unsigned long)g_daisyBytes,
                          (unsigned long)g_daisyLines, gpsLine);
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[ais-radar] display + touch + GPS + Daisy AIS (ADR-0016)");

    const bool haveDisplay = amoled::begin();
    touch::begin();
    gps::begin();   // UART-only; silent until R15/R16 jumpers are soldered

    // Daisy 2+ AIS on IO16 (header H2). RX only (TX unused).
    Serial2.setRxBufferSize(2048);
    Serial2.begin(kDaisyBaud, SERIAL_8N1, /*rx=*/16, /*tx=*/-1);
    Serial.printf("[daisy] Serial2 @ %lu on IO16 — listening\n",
                  (unsigned long)kDaisyBaud);

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
    static uint32_t last = 0, lastRefresh = 0, lastStat = 0;
    const uint32_t now = millis();

    daisyPoll();
    gps::poll(now);
    int16_t tx, ty;
    (void)touch::readPoint(tx, ty);

    lv_tick_inc(now - last);
    last = now;
    if (now - lastRefresh >= 500) { lastRefresh = now; refreshSummary(); }
    if (now - lastStat >= 2000) {
        lastStat = now;
        Serial.printf("[stat] ais_bytes=%lu lines=%lu targets=%u | last: %s\n",
                      (unsigned long)g_daisyBytes, (unsigned long)g_daisyLines,
                      (unsigned)decoder.store().size(), g_lastLine);
    }
    lv_timer_handler();
    delay(5);
}
