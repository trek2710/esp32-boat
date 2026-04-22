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

// Bump the Arduino loopTask stack from the 8 KB default to 16 KB.
//
// ESP-IDF's spi_bus_initialize (which our ST77916 panel driver calls during
// ui::begin()) uses several KB of stack on its own, and LVGL widget
// construction for the three UI pages piles on top of that. With the default
// stack the S3 hits FreeRTOS's stack-overflow detector mid-setup and
// reboots in a loop. 16 KB is comfortable headroom for both and costs us
// only 8 KB of RAM.
//
// SET_LOOP_TASK_STACK_SIZE is a framework macro that the Arduino-ESP32
// core checks when it creates loopTask — it must be at global scope.
#if !DISPLAY_SAFE_MODE
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
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

    // USB-CDC-on-boot needs ~1.5–2 s to fully enumerate on the host before
    // the first byte is guaranteed to reach the terminal. Without this, a
    // crash later in setup() can drop the CDC endpoint and all prior output
    // with it, making the boot loop look like silence. 2 s is conservative.
    delay(2000);

    // We use log_i() rather than Serial.println() for the diagnostic output
    // below. Arduino's HWCDC-backed `Serial` (active whenever
    // ARDUINO_USB_CDC_ON_BOOT=1) silently drops writes when the host-side
    // CDC endpoint isn't actively reading — which is exactly the case for
    // the first few hundred ms after reset, while the host is
    // re-enumerating. That was masking *all* of our setup() progress logs
    // on a freshly flashed image. log_i goes through the lower-level ESP-IDF
    // console path (same path as the esp32-hal-* core and esp_log messages
    // we see reliably) and is not gated on CDC-connected state.
    log_i("esp32-boat booting");

#if DISPLAY_SAFE_MODE
    log_i("[SAFE MODE] display init skipped (DISPLAY_SAFE_MODE=1)");
#else
    log_i("[step] ui::begin()");
    ui::begin(g_state);
    log_i("[step] ui::begin() ok");
#endif

    log_i("[step] nmea::begin()");
    if (!g_bridge.begin()) {
        log_w("[WARN] NMEA 2000 bridge failed to start - continuing with no bus.");
    }

#if !DISPLAY_SAFE_MODE
    xTaskCreatePinnedToCore(lvglTickTask, "lvgl-tick", 2048, nullptr, 1, nullptr, 1);
#endif

    log_i("esp32-boat ready");
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
