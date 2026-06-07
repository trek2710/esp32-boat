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

static void showSummary() {
    const size_t targets = decoder.store().size();
    lv_obj_t* label = lv_label_create(lv_scr_act());
    lv_label_set_text_fmt(label, "AIS-radar\n%u targets\n%llu msgs",
                          (unsigned)targets,
                          (unsigned long long)decoder.messages());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n[ais-radar] milestone 0 — AMOLED + AIS decode salvage (ADR-0016)");

    const bool haveDisplay = amoled::begin();

    // Verified sentences (correct checksums) from the AIS test-sentence set.
    feed("!AIVDM,1,1,,B,B52K>7008h>KUH7v8L0L;wv00000,0*59");  // type 18  Class B position
    feed("!AIVDM,1,1,,A,15Memvh01E0qcJ0Op:D:ggwp00000,5*2C");  // type 1   Class A position
    feed("!AIVDM,2,1,1,A,55Memvh2;HNMMA@h001@U@4r0<58Lt0000000016000000000Bhkl1CR0AiC,0*53"); // type 5 (1/2)
    feed("!AIVDM,2,2,1,A,P0000000000,2*45");                   // type 5   (2/2) name + type

    Serial.printf("[ais] %u target(s) from %llu message(s)\n",
                  (unsigned)decoder.store().size(),
                  (unsigned long long)decoder.messages());

    if (haveDisplay) showSummary();
    else Serial.println("[ais-radar] display unavailable — serial only");
}

void loop() {
    static uint32_t last = 0;
    const uint32_t now = millis();
    lv_tick_inc(now - last);
    last = now;
    lv_timer_handler();
    delay(5);
}
