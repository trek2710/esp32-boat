// st77916_panel.h — ST77916 QSPI panel driver for the Waveshare
// ESP32-S3-Touch-LCD-2.1 board's 480×480 round IPS display.
//
// Why we rolled this by hand:
//   - LovyanGFX's stock panel list doesn't include ST77916.
//   - Arduino-ESP32 2.0.16 ships ESP-IDF 4.4, which pre-dates esp_lcd's
//     generic QSPI LCD panel_io (that landed in IDF 5.0). So we drive the
//     QSPI bus via ESP-IDF's spi_master driver directly, using quad
//     transactions for both command+address and pixel payload.
//   - Doing it ourselves keeps the dep list tiny (no LovyanGFX, no esp-bsp)
//     and the bring-up path visible in two files you can actually read.
//
// What it does:
//   begin(Tca9554&)
//       Owns bus init, device add, TCA9554-gated reset sequence, vendor init
//       register writes, and turns the backlight on at the end. After this
//       returns successfully the panel is showing whatever GRAM held at
//       power-on (usually noise) — you must then push a frame with
//       drawBitmap() or LVGL's flush_cb.
//
//   drawBitmap(x1, y1, x2, y2, pixels)
//       Stream a rectangle of RGB565 pixels to the panel. Thin wrapper
//       around `write_pixels` that also sets the CASET/RASET address
//       window. Coordinates are inclusive at both ends, matching LVGL's
//       lv_area_t convention.

#pragma once

#include <cstddef>
#include <cstdint>

namespace display {

class Tca9554;  // forward decl; see tca9554.h

class St77916Panel {
public:
    // Bring the panel out of reset and run the vendor init sequence. Returns
    // true on success; false means "I tried but something didn't ACK / SPI
    // init failed — don't expect pixels".
    bool begin(Tca9554& io);

    // Push a rectangle of RGB565 pixels. Byte-order matches LVGL's
    // LV_COLOR_16_SWAP=1 layout (high byte first on the wire).
    // (x1,y1)-(x2,y2) inclusive. Blocks until DMA is done.
    void drawBitmap(int x1, int y1, int x2, int y2, const void* pixels);

    // Convenience — fill entire screen with one RGB565 color. Used by
    // post-init "prove the panel is alive" smoke tests.
    void fillColor(uint16_t rgb565);

    bool isReady() const { return ready_; }

private:
    bool initBus();
    bool resetPanel(Tca9554& io);
    bool runInitSequence();
    void sendCommand(uint8_t cmd);
    void sendCommand(uint8_t cmd, const uint8_t* data, size_t len);
    void sendPixels(const void* pixels, size_t bytes);

    void* spi_dev_ = nullptr;   // spi_device_handle_t, opaque to avoid pulling
                                // ESP-IDF headers into every .cpp that sees us
    bool  ready_   = false;
};

}  // namespace display
