// st7701_panel.h — ST7701 RGB-parallel panel driver for the Waveshare
// ESP32-S3-Touch-LCD-2.1 board's 480×480 round IPS display (ST7701 + TCA9554
// variant — see display_pins.h for the chipset-pivot story).
//
// Why roll this by hand (same philosophy as the superseded st77916_panel.h):
//   - The Waveshare reference demo pulls in a large "LCD_Driver" component
//     with its own I2C / SPI helpers that replicate what we already have
//     in tca9554.* and Wire. Keeping the driver local lets us reuse those.
//   - LovyanGFX's Panel_ST7701 expects the panel's CS line to be a real
//     ESP32 GPIO. On this board CS is gated by the TCA9554 I/O expander,
//     which doesn't fit that API — see LovyanGFX discussion #630 where
//     several users hit the same wall and eventually switched to
//     esp_lcd_panel_rgb. We go directly to that.
//   - Arduino-ESP32 2.0.16 (ESP-IDF 4.4.7) ships with esp_lcd_panel_rgb.h
//     in its bundled IDF fork, so we don't need to bump toolchains.
//
// Control flow at begin(Tca9554&):
//   1. Hold the panel in reset via TCA9554 IO1 (LCD_RST active low).
//   2. Release reset, wait 120 ms for the ST7701 internal PLL to come up.
//   3. Bit-bang the vendor init sequence over a 3-wire SPI link built out
//      of two spare GPIOs (SCK=GPIO2, SDA=GPIO1) and a TCA9554-driven CS.
//      Each transaction is a 9-bit word on the wire: one DCX bit
//      (0=command, 1=data), then the 8-bit value MSB-first.
//   4. Configure esp_lcd_new_rgb_panel with our pin map + porch timings
//      and hand it a PSRAM-backed framebuffer big enough for 480×480×2.
//   5. esp_lcd_panel_init pushes the first frame and starts PCLK.
//   6. Turn the backlight on via PWM on RAW GPIO6 (ledcSetup + ledcAttachPin
//      + ledcWrite at full duty) last, so the "panel comes alive" moment
//      is the blacked-out framebuffer, not whatever initial DRAM garbage
//      the display was showing. Round 14 tried to drive the backlight
//      through TCA9554 IO0 — that was wrong on two counts: the backlight
//      isn't on the expander at all, AND the bit we wrote happens to be
//      the panel-reset line (LCD_RST on IO0 in Waveshare's 1-indexed EXIO
//      naming), so it was actively pulsing RESET while expecting pixels.
//
// drawBitmap(...) is a thin wrapper over esp_lcd_panel_draw_bitmap, same
// signature as st77916_panel.h so Ui.cpp's flush_cb doesn't care which
// panel driver is active.

#pragma once

#include <cstddef>
#include <cstdint>

namespace display {

class Tca9554;  // forward decl; see tca9554.h

class St7701Panel {
public:
    // Run the full bring-up sequence above. Returns true only if every step
    // (reset pulse, all 37 init commands, esp_lcd_panel_init) succeeded.
    // Returning false leaves the panel in an undefined state — caller should
    // treat subsequent drawBitmap calls as no-ops.
    bool begin(Tca9554& io);

    // Push a rectangle of RGB565 pixels to the framebuffer. Byte order
    // matches LVGL's LV_COLOR_16_SWAP=1 layout (high byte first on the
    // wire). (x1,y1)-(x2,y2) inclusive — matches lv_area_t. Returns
    // immediately; esp_lcd_panel_rgb handles its own PCLK/DMA clocking in
    // the background off the framebuffer we just stomped.
    void drawBitmap(int x1, int y1, int x2, int y2, const void* pixels);

    // Paint the entire screen a single RGB565 color. Used for bring-up
    // smoke tests and the "prove LVGL can flush a full frame" step in
    // Ui.cpp::begin().
    void fillColor(uint16_t rgb565);

    // Round 35: block until the panel's RGB DMA finishes the current frame
    // (i.e. is at the start of vertical blanking with the next row-0 scan
    // ~vsync_pulse_width + vsync_back_porch microseconds away). Call this
    // immediately before drawBitmap to ensure the framebuffer memcpy starts
    // at the top of a fresh frame and has the maximum runway to stay ahead
    // of the ongoing scanout — without this, drawBitmap and the DMA scan
    // race each other within the single shared PSRAM framebuffer (IDF 4.4.7
    // only allocates one) and the panel sees a mix of old + new rows for
    // one or two frames per flush, which is exactly the side-to-side
    // shimmer the round 33 / 34 photos and IMG_1907.MOV showed on the
    // dial labels and needle.
    //
    // Implementation: cfg.on_frame_trans_done in initRgbPanel() gives a
    // binary semaphore from ISR; this method takes it with a 50 ms
    // timeout (about 3 frames) so a missed callback can't stall the UI
    // task indefinitely.
    void waitForVsync();

    bool isReady() const { return ready_; }

private:
    // 3-wire SPI init helpers. These operate through the TCA9554 for CS
    // and on raw GPIOs for SCK + SDA — slow (each CS toggle is an I2C
    // write, ~500 µs on a 100 kHz bus) but only runs once at boot, so
    // the ~100 ms total cost is a non-issue.
    bool resetPanel(Tca9554& io);
    bool runInitSequence(Tca9554& io);
    void spiWriteByte(Tca9554& io, bool is_data, uint8_t value);

    // ESP-IDF RGB panel bring-up.
    bool initRgbPanel();

    // esp_lcd_panel_handle_t, kept opaque so callers don't have to pull in
    // ESP-IDF's esp_lcd headers.
    void* panel_ = nullptr;
    bool  ready_ = false;
};

}  // namespace display
