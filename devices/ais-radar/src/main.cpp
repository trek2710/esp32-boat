// AIS-radar device (ADR-0016) — display + touch + GPS + live Daisy AIS,
// drawn radar-style on the AMOLED.
//
// Reads the Wegmatt dAISy 2+ on IO16 (Serial2 @ 38400), decodes AIVDM into
// the target store, and plots targets around own ship. Own position comes
// from the LC76G once R15/R16 are soldered; until then it's pinned to a
// fixed bench location so targets place correctly. Targets expire after 10 s
// of silence. Next: the BLE link to iOS.

#include <Arduino.h>
#include <lvgl.h>
#include <AmoledDisplay.h>
#include <Touch.h>
#include <Lc76gGps.h>
#include <AisTargetDecoder.h>
#include <default_sentence_parser.h>
#include "Radar.h"
#include "Ble.h"

static constexpr uint32_t kDaisyBaud  = 38400;   // dAISy NMEA-HS default
static constexpr uint32_t kTargetLifeMs = 10000; // drop targets after 10 s

// Bench own-ship position, used until the LC76G GPS is soldered/working.
static constexpr double kBenchLat = 55.76196;
static constexpr double kBenchLon = 12.62900;

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
static char     g_line[120];
static int      g_len = 0;

static void daisyPoll() {
    while (Serial2.available() > 0) {
        char c = (char)Serial2.read();
        g_daisyBytes++;
        if (c == '\n') {
            if (g_len > 0) { g_line[g_len] = '\0'; g_daisyLines++; feed(g_line); }
            g_len = 0;
        } else if (c != '\r' && g_len < (int)sizeof(g_line) - 1) {
            g_line[g_len++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[ais-radar] radar UI + Daisy AIS (ADR-0016)");

    const bool haveDisplay = amoled::begin();
    touch::begin();
    gps::begin();   // UART-only; silent until R15/R16 jumpers are soldered

    Serial2.setRxBufferSize(2048);
    Serial2.begin(kDaisyBaud, SERIAL_8N1, /*rx=*/16, /*tx=*/-1);
    Serial.printf("[daisy] Serial2 @ %lu on IO16\n", (unsigned long)kDaisyBaud);

    if (haveDisplay) radar::begin();
    else Serial.println("[ais-radar] display unavailable — serial only");

    ble::begin();
}

void loop() {
    static uint32_t last = 0, lastDraw = 0, lastStat = 0, lastBle = 0;
    const uint32_t now = millis();

    daisyPoll();
    gps::poll(now);
    int16_t tx, ty;
    (void)touch::readPoint(tx, ty);

    lv_tick_inc(now - last);
    last = now;

    if (now - lastDraw >= 500) {
        lastDraw = now;
        decoder.store().evictStale(kTargetLifeMs);   // 10 s lifetime
        const gps::Fix& f = gps::fix();
        const bool haveFix = (f.fixQuality >= 1) && !isnan(f.lat) && !isnan(f.lon);
        const double lat = haveFix ? f.lat : kBenchLat;
        const double lon = haveFix ? f.lon : kBenchLon;
        radar::draw(decoder.store(), lat, lon, NAN /*own cog unknown*/);
    }
    if (now - lastBle >= 1000) {
        lastBle = now;
        const gps::Fix& f = gps::fix();
        const bool haveFix = (f.fixQuality >= 1) && !isnan(f.lat) && !isnan(f.lon);
        ble::publish(decoder.store(), haveFix ? f.lat : kBenchLat,
                     haveFix ? f.lon : kBenchLon, NAN, haveFix);
    }
    if (now - lastStat >= 2000) {
        lastStat = now;
        Serial.printf("[stat] ais_bytes=%lu lines=%lu targets=%u ble=%d\n",
                      (unsigned long)g_daisyBytes, (unsigned long)g_daisyLines,
                      (unsigned)decoder.store().size(), ble::connected());
    }
    lv_timer_handler();
    delay(5);
}
