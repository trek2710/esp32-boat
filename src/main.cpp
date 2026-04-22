// esp32-boat — entry point.
//
// Arduino framework. setup() brings up the display + NMEA bridge, loop() runs
// the LVGL tick; the NMEA parse loop runs in its own FreeRTOS task on core 0.

#include <Arduino.h>

#include "BoatState.h"
#include "NmeaBridge.h"
#include "Ui.h"
#include "config.h"

// LVGL is only pulled in when we actually drive the display. Safe mode has no
// UI, and we're separately debugging a header-visibility issue with <lvgl.h>
// (lv_color_t / lv_obj_t / lv_tick_inc not declared when compiling our files).
// Keeping lvgl.h out of the safe build means safe-mode firmware can be
// flashed and observed on real hardware without being blocked on that.
// NOTE: we include <lvgl/lvgl.h> rather than plain <lvgl.h>. LovyanGFX ships
// its own minimal lvgl.h shim at .pio/libdeps/<env>/LovyanGFX/src/lvgl.h, and
// LDF puts that include path BEFORE the real LVGL's, so <lvgl.h> resolves to
// the shim and only forward-declares lv_font_t. Going via the lvgl/ subdir
// hits the real header unambiguously (only real LVGL has lvgl/lvgl.h). See
// the long comment in platformio.ini for the full explanation.
#if !DISPLAY_SAFE_MODE
#include <lvgl/lvgl.h>
#endif

namespace {
BoatState   g_state;
NmeaBridge  g_bridge(g_state);

#if !DISPLAY_SAFE_MODE
// lv_tick_inc hook — called from a hardware timer via a small task. Only
// needed when there's actually a UI to tick.
void lvglTickTask(void*) {
    for (;;) {
        lv_tick_inc(LV_TICK_MS);
        vTaskDelay(pdMS_TO_TICKS(LV_TICK_MS));
    }
}
#endif
}  // namespace

void setup() {
    Serial.begin(115200);

    // USB-CDC-on-boot needs ~1.5–2 s to fully enumerate on the host before our
    // first printf is guaranteed to reach the terminal. Without this, a crash
    // later in setup() can drop the CDC endpoint and all prior output with it,
    // making the boot loop look like silence. 2 s is conservative but cheap.
    delay(2000);
    Serial.println(F("esp32-boat booting"));
    Serial.flush();

#if DISPLAY_SAFE_MODE
    Serial.println(F("[SAFE MODE] display init skipped (DISPLAY_SAFE_MODE=1)"));
    Serial.flush();
#else
    Serial.println(F("[step] ui::begin()"));
    Serial.flush();
    ui::begin(g_state);
    Serial.println(F("[step] ui::begin() ok"));
    Serial.flush();
#endif

    Serial.println(F("[step] nmea::begin()"));
    Serial.flush();
    if (!g_bridge.begin()) {
        Serial.println(F("[WARN] NMEA 2000 bridge failed to start — continuing with no bus."));
    }

#if !DISPLAY_SAFE_MODE
    xTaskCreatePinnedToCore(lvglTickTask, "lvgl-tick", 2048, nullptr, 1, nullptr, 1);
#endif

    Serial.println(F("esp32-boat ready"));
    Serial.flush();
}

void loop() {
#if SIMULATED_DATA
    g_bridge.simulateTick();
#endif

#if DISPLAY_SAFE_MODE
    // No LVGL to tick in safe mode — just emit a heartbeat so serial shows
    // the firmware really is running end-to-end.
    static uint32_t last_beat = 0;
    if (millis() - last_beat > 1000) {
        last_beat = millis();
        Serial.printf("[safe-mode heartbeat] t=%lu ms, free heap=%lu\n",
                      static_cast<unsigned long>(last_beat),
                      static_cast<unsigned long>(ESP.getFreeHeap()));
    }
    delay(20);
#else
    const uint32_t wait = ui::tick();
    delay(wait < 5 ? 5 : (wait > 20 ? 20 : wait));
#endif
}
