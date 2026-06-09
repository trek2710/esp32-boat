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
#include <SD_MMC.h>
#include <AmoledDisplay.h>
#include <Touch.h>
#include <Lc76gGps.h>
#include <AisTargetDecoder.h>
#include <default_sentence_parser.h>
#include "Radar.h"
#include "Ble.h"
#include "DeviceSettings.h"

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
static int      g_threat = 0;          // 0=none 1=safe 2=alert 3=danger
static int      g_batt = -1;           // ESP battery % (AXP2101), <0 = unknown
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

// One-shot SD-card recon (C-MAP investigation). Mounts the onboard microSD
// (SD_MMC, 1-bit: CLK=2 CMD=1 DATA=3 per Waveshare pin_config) and lists the
// root + cmap/ over serial so we can see the card layout + format.
static void listSdDir(const String& path, int depth, int maxDepth) {
    File d = SD_MMC.open(path);
    if (!d || !d.isDirectory()) return;
    int count = 0;
    for (File e = d.openNextFile(); e && count < 16; e = d.openNextFile(), ++count) {
        const char* name = e.name();
        if (name[0] == '.') continue;   // skip macOS cruft (._, .Spotlight…)
        for (int i = 0; i < depth; ++i) Serial.print("  ");
        Serial.printf("%s%s  %lu B\n", name, e.isDirectory() ? "/" : "",
                      (unsigned long)e.size());
        if (e.isDirectory() && depth < maxDepth) {
            String sub = (path == "/") ? ("/" + String(name)) : (path + "/" + name);
            listSdDir(sub, depth + 1, maxDepth);
        }
    }
}

static void listSdCard() {
    SD_MMC.setPins(2, 1, 3);
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("[sd] mount FAILED");
        return;
    }
    Serial.printf("[sd] mounted: type=%d size=%lluMB\n[sd] tree (2 levels):\n",
                  (int)SD_MMC.cardType(),
                  (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
    listSdDir("/", 0, 2);
    Serial.println("[sd] end");
}

// Own-ship state. Priority: onboard LC76G fix > phone GPS over BLE > bench.
struct Own { double lat, lon, cog, sog; bool realFix; };
static Own ownShip() {
    const gps::Fix& f = gps::fix();
    ble::HostGps hg;
    // Phone GPS preferred when enabled + fresh (even over the LC76G, in case the
    // phone is more accurate); otherwise LC76G; otherwise the bench coord.
    if (devsettings::get().phoneGps && ble::hostGps(&hg)) {
        return { hg.lat, hg.lon, hg.cogDeg, isnan(hg.sogKn) ? 0.0 : hg.sogKn, true };
    }
    if (f.fixQuality >= 1 && !isnan(f.lat) && !isnan(f.lon)) {
        return { f.lat, f.lon, NAN, 0.0, true };          // LC76G (position only)
    }
    return { kBenchLat, kBenchLon, NAN, 0.0, false };     // bench fallback
}

// Bench test targets: three Class-B vessels at FIXED positions near the bench
// (absolute, like real AIS — they don't follow own ship). Showing the three
// threat modes when own ship is at the bench. Re-stamped each call so they
// never evict. Go to the AMOLED and, via ble::publish, the iOS app.
static void injectTestTargets() {
    struct T { uint32_t mmsi; const char* nm; uint8_t type;
               float sog, cog; double dLat, dLon; };
    static const T ts[] = {
        {992110001, "SAILBOAT", 36,  5.0f, 210.0f,  0.0480,  0.0300},  // safe   — triangle
        {992110002, "YACHT",    37, 22.0f, 120.0f,  0.0200, -0.0250},  // alert  — circle
        {992110003, "TANKER",   80, 10.0f, 180.0f,  0.0133,  0.0000},  // danger — hull
    };
    for (const auto& t : ts) {
        decoder.store().recordName(t.mmsi, 'B', t.nm);
        decoder.store().recordType(t.mmsi, 'B', t.type);
        decoder.store().recordPosition(t.mmsi, 'B', t.sog, t.cog);
        decoder.store().recordLatLon(t.mmsi, kBenchLat + t.dLat, kBenchLon + t.dLon);
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[ais-radar] radar UI + Daisy AIS (ADR-0016)");
    devsettings::load();

    const bool haveDisplay = amoled::begin();
    touch::begin();
    gps::begin();   // UART-only; silent until R15/R16 jumpers are soldered

    Serial2.setRxBufferSize(2048);
    Serial2.begin(kDaisyBaud, SERIAL_8N1, /*rx=*/16, /*tx=*/-1);
    Serial.printf("[daisy] Serial2 @ %lu on IO16\n", (unsigned long)kDaisyBaud);

    if (haveDisplay) radar::begin();
    else Serial.println("[ais-radar] display unavailable — serial only");

    ble::begin();
    listSdCard();   // one-shot C-MAP recon (late so it survives USB re-enum)
}

void loop() {
    static uint32_t last = 0, lastDraw = 0, lastStat = 0, lastBle = 0, lastBatt = 0;
    const uint32_t now = millis();

    daisyPoll();
    gps::poll(now);
    int16_t tx, ty;
    (void)touch::readPoint(tx, ty);

    lv_tick_inc(now - last);
    last = now;

    if (now - lastBatt >= 5000) { lastBatt = now; g_batt = amoled::batteryPercent(); }

    if (now - lastDraw >= 500) {
        lastDraw = now;
        Own o = ownShip();
        if (devsettings::get().testTargets) injectTestTargets();
        decoder.store().evictStale(kTargetLifeMs);   // 10 s lifetime
        radar::draw(decoder.store(), o.lat, o.lon, o.cog, o.sog, g_batt);
        g_threat = radar::assessWorst(decoder.store(), o.lat, o.lon, o.cog, o.sog);
    }
    if (now - lastBle >= 1000) {
        lastBle = now;
        Own o = ownShip();
        ble::publish(decoder.store(), o.lat, o.lon, o.cog, o.sog, o.realFix, g_threat, g_batt);
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
