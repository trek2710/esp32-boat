// esp32-boat — entry point.
//
// Arduino framework. setup() brings up the display + NMEA bridge, loop() runs
// the LVGL tick; the NMEA parse loop runs in its own FreeRTOS task on core 0.

#include <Arduino.h>

#include "BoatState.h"
#include "NmeaBridge.h"
#include "Ui.h"
#include "config.h"

namespace {
BoatState   g_state;
NmeaBridge  g_bridge(g_state);

// lv_tick_inc hook — called from a hardware timer via a small task.
void lvglTickTask(void*) {
    for (;;) {
        lv_tick_inc(LV_TICK_MS);
        vTaskDelay(pdMS_TO_TICKS(LV_TICK_MS));
    }
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println(F("esp32-boat booting"));

    ui::begin(g_state);

    if (!g_bridge.begin()) {
        Serial.println(F("[WARN] NMEA 2000 bridge failed to start — continuing with no bus."));
    }

    xTaskCreatePinnedToCore(lvglTickTask, "lvgl-tick", 2048, nullptr, 1, nullptr, 1);

    Serial.println(F("esp32-boat ready"));
}

void loop() {
#if SIMULATED_DATA
    g_bridge.simulateTick();
#endif
    const uint32_t wait = ui::tick();
    delay(wait < 5 ? 5 : (wait > 20 ? 20 : wait));
}
