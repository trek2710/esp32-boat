#include "AmoledDisplay.h"

#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

// Salvaged verbatim (behaviour-preserving) from src_tx/main.cpp's
// initDisplay() / flushDisplayCb() / initLvgl(), minus the app UI. See that
// file's commit history (tag v1-wifi-bus-archive) for the bring-up notes —
// the CO5300-for-SH8601 substitution and the 6-column window offset in
// particular were found the hard way.

namespace amoled {
namespace {

constexpr uint8_t kAxp2101Addr = 0x34;
constexpr int kLcdCs = 12, kLcdSck = 38;
constexpr int kLcdD0 = 4, kLcdD1 = 5, kLcdD2 = 6, kLcdD3 = 7;
constexpr int kLcdReset = 39;

XPowersAXP2101    PMU;
Arduino_DataBus  *gfxBus = nullptr;
Arduino_GFX      *gfx    = nullptr;

lv_disp_draw_buf_t lvDrawBuf;
lv_color_t        *lvBuf = nullptr;
lv_disp_drv_t      lvDispDrv;

void flushCb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    const int16_t w = area->x2 - area->x1 + 1;
    const int16_t h = area->y2 - area->y1 + 1;
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(disp);
}

bool initDisplay() {
    gfxBus = new Arduino_ESP32QSPI(kLcdCs, kLcdSck,
                                   kLcdD0, kLcdD1, kLcdD2, kLcdD3);
    // CO5300 driver stands in for the pin-compatible SH8601; the 6-column
    // window offset avoids the green seam on these round 466×466 AMOLEDs.
    gfx = new Arduino_CO5300(gfxBus, kLcdReset, 0 /*rotation*/, false /*ips*/,
                             kLcdW, kLcdH, 6, 0, 6, 0);
    if (!gfx->begin()) {
        Serial.println("[amoled] gfx->begin() failed — check QSPI pins / panel power");
        return false;
    }
    gfx->fillScreen(BLACK);
    return true;
}

bool initLvgl() {
    lv_init();
    const size_t pixels = (size_t)kLcdW * kLcdH;
    lvBuf = (lv_color_t *)heap_caps_malloc(pixels * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM);
    if (!lvBuf) {
        Serial.println("[amoled] PSRAM draw-buffer alloc failed");
        return false;
    }
    lv_disp_draw_buf_init(&lvDrawBuf, lvBuf, nullptr, pixels);
    lv_disp_drv_init(&lvDispDrv);
    lvDispDrv.hor_res  = kLcdW;
    lvDispDrv.ver_res  = kLcdH;
    lvDispDrv.flush_cb = flushCb;
    lvDispDrv.draw_buf = &lvDrawBuf;
    lv_disp_drv_register(&lvDispDrv);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);
    return true;
}

}  // namespace

bool begin() {
    if (!Wire.begin(kI2cSda, kI2cScl, 100000)) {
        Serial.println("[amoled] Wire.begin() failed");
        return false;
    }
    if (!PMU.begin(Wire, kAxp2101Addr, kI2cSda, kI2cScl)) {
        Serial.println("[amoled] PMU.begin() failed (AXP2101 @ 0x34)");
        return false;
    }
    if (!initDisplay()) return false;
    if (!initLvgl())    return false;
    Serial.println("[amoled] display + LVGL up (466×466 RGB565)");
    return true;
}

int batteryPercent() {
    return PMU.getBatteryPercent();
}

}  // namespace amoled
