// AIS-radar device — milestone 0: AIS decode salvage test (ADR-0016).
//
// Proves the salvaged AIVDM decoder (shared/ais) compiles and runs on the
// new per-device tree, decoding verified test sentences into the target
// store — including target lat/lon, which the v1 decoder never recorded.
//
// Next steps (see docs/ROADMAP.md "v2 — Device 1"): AMOLED + LC76G GPS
// bring-up, Daisy AIS on a UART, LVGL radar render, BLE link to iOS.

#include <Arduino.h>
#include <AisTargetDecoder.h>
#include <default_sentence_parser.h>

static AisTargetDecoder decoder;
static AIS::DefaultSentenceParser parser;

// Feed one raw AIVDM sentence through the decoder. decodeMsg must be called
// until it returns 0; multi-fragment messages (e.g. type 5) are reassembled
// across successive feed() calls because the decoder buffers fragments.
static void feed(const char* sentence) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", sentence);
    size_t i = 0;
    do { i = decoder.decodeMsg(buf, (size_t)n, i, parser); } while (i != 0);
}

static void dumpTargets() {
    AisTarget t[AisTargetStore::CAPACITY];
    const size_t n = decoder.store().snapshotByRecency(t, AisTargetStore::CAPACITY);
    Serial.printf("[ais] %u target(s) from %llu decoded message(s):\n",
                  (unsigned)n, (unsigned long long)decoder.messages());
    for (size_t i = 0; i < n; ++i) {
        Serial.printf("  MMSI %-9u cls%c  %-20s  lat %.5f lon %.5f  "
                      "sog %.1f cog %.0f  type %u nav %u\n",
                      (unsigned)t[i].mmsi, t[i].klass,
                      t[i].name[0] ? t[i].name : "(no name)",
                      t[i].lat_deg, t[i].lon_deg, t[i].sog_kn, t[i].cog_deg,
                      t[i].vessel_type, t[i].nav_status);
    }
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[ais-radar] device skeleton — AIS decode salvage test (ADR-0016)");

    // Verified sentences (correct checksums) from the AIS test-sentence set.
    feed("!AIVDM,1,1,,B,B52K>7008h>KUH7v8L0L;wv00000,0*59");  // type 18  Class B position
    feed("!AIVDM,1,1,,A,15Memvh01E0qcJ0Op:D:ggwp00000,5*2C");  // type 1   Class A position
    feed("!AIVDM,2,1,1,A,55Memvh2;HNMMA@h001@U@4r0<58Lt0000000016000000000Bhkl1CR0AiC,0*53"); // type 5 (1/2)
    feed("!AIVDM,2,2,1,A,P0000000000,2*45");                   // type 5   (2/2) name + type

    dumpTargets();
    Serial.println("[ais-radar] done.");
}

void loop() {
    delay(1000);
}
