// st77916_panel.cpp — see st77916_panel.h for the overview.
//
// Protocol reminder for ST77916 over QSPI:
//
//   Command frame:
//       [CMD=0x02] [24-bit addr: 0x00 | <8-bit cmd> | 0x00] [payload bytes...]
//       Command + address are clocked on a single SPI line; payload is
//       clocked on 4 lines (quad) if any.
//
//   Pixel-write frame (memory write accelerated variant):
//       [CMD=0x32] [24-bit addr: 0x00 | 0x2C | 0x00] [pixel stream in QUAD]
//       Entire transaction including command + address runs in quad mode.
//
// We implement both with `spi_transaction_ext_t` and the SPI_TRANS_MODE_QIO
// flag, varying command_bits / address_bits / flags per call.

#include "st77916_panel.h"

#include <Arduino.h>
#include <cstring>

#include "driver/spi_master.h"

#include "ch422g.h"
#include "display_pins.h"

namespace display {

namespace {

// Helper to hold a pointer handle to the SPI device without leaking
// IDF types into the public header.
spi_device_handle_t& asHandle(void*& p) {
    return *reinterpret_cast<spi_device_handle_t*>(&p);
}

// ─────────────────────────────────────────────────────────────────────────
// Vendor init sequence.
//
// This is a generic "minimum viable" ST77916 bring-up: software reset,
// sleep out, 16-bit color, display on. It's enough to reliably make the
// panel accept pixel writes on most ST77916 dev boards.
//
// However, boards like the Waveshare 2.1" typically need a LONGER
// vendor-specific init (gamma curves, VCOM, power sequencing, bond-out
// registers) to get correct colors and clean contrast. Waveshare ships
// that table in their demo under:
//     examples/*/components/esp_lcd_panel_st77916/esp_lcd_st77916.c
// When you have that handy, replace the kInit[] table below with theirs.
//
// Each entry is: { register, paramCount, {paramBytes...}, delayMs }.
// Entries are walked linearly by runInitSequence().
// ─────────────────────────────────────────────────────────────────────────
struct InitCmd {
    uint8_t cmd;
    uint8_t paramCount;      // 0..16
    uint8_t params[16];
    uint16_t delayMs;        // post-command delay (0 = none)
};

constexpr InitCmd kInit[] = {
    // Software reset. Needs >=120 ms afterwards before any further command
    // (ST77916 datasheet § "Reset Timing").
    { 0x01, 0, {}, 150 },

    // Sleep out. Needs >=120 ms before the charge pumps are stable.
    { 0x11, 0, {}, 150 },

    // MADCTL — memory access / RGB-BGR order. 0x00 = default orientation,
    // RGB. If colors look swapped on your board flip bit 3 to 0x08 (BGR).
    { 0x36, 1, { 0x00 }, 0 },

    // COLMOD — interface pixel format. 0x55 = 16 bpp / RGB565.
    { 0x3A, 1, { 0x55 }, 0 },

    // Inversion on. Most ST77916 round panels ship with the pixel polarity
    // requiring INVON; if the image comes out as a photo negative, remove.
    { 0x21, 0, {}, 0 },

    // Normal display mode.
    { 0x13, 0, {}, 0 },

    // Display on.
    { 0x29, 0, {}, 50 },
};

}  // namespace

// ── Public API ────────────────────────────────────────────────────────────

bool St77916Panel::begin(Ch422g& io) {
    // Use log_i / log_e (ESP-IDF log path via ets_printf) rather than
    // Serial.println. With ARDUINO_USB_CDC_ON_BOOT=1 the Arduino `Serial`
    // is a HWCDC instance whose write() silently drops bytes when the
    // host-side CDC endpoint isn't actively reading — which happens for
    // ~hundreds of ms right after reset, exactly when we most want these
    // logs. The ESP-IDF console path (used by log_i) doesn't have that
    // gate, so these show up reliably on the first boot of a
    // freshly-flashed image. The ESP_LOGE "txdata transfer > ..." errors
    // from the SPI driver that we were seeing went through this same
    // path, which is why they were visible and ours weren't.
    log_i("[st77916] initBus");
    if (!initBus())         { log_e("[st77916] initBus failed");         return false; }
    log_i("[st77916] resetPanel");
    if (!resetPanel(io))    { log_e("[st77916] resetPanel failed");      return false; }
    log_i("[st77916] runInitSequence");
    if (!runInitSequence()) { log_e("[st77916] runInitSequence failed"); return false; }
    log_i("[st77916] backlight on");

    // Flip the backlight on (CH422G EXIO0). Do this AFTER the init sequence
    // so the operator doesn't see a flash of power-on noise.
    io.setBits(CH422G_BIT_LCD_BL);

    ready_ = true;
    return true;
}

void St77916Panel::drawBitmap(int x1, int y1, int x2, int y2,
                              const void* pixels) {
    if (!ready_) return;

    const uint8_t caset[4] = {
        static_cast<uint8_t>((x1 >> 8) & 0xFF),
        static_cast<uint8_t>(x1 & 0xFF),
        static_cast<uint8_t>((x2 >> 8) & 0xFF),
        static_cast<uint8_t>(x2 & 0xFF),
    };
    const uint8_t raset[4] = {
        static_cast<uint8_t>((y1 >> 8) & 0xFF),
        static_cast<uint8_t>(y1 & 0xFF),
        static_cast<uint8_t>((y2 >> 8) & 0xFF),
        static_cast<uint8_t>(y2 & 0xFF),
    };
    sendCommand(0x2A, caset, 4);  // CASET: column address set
    sendCommand(0x2B, raset, 4);  // RASET: row address set

    const size_t pxcount = static_cast<size_t>((x2 - x1 + 1) * (y2 - y1 + 1));
    sendPixels(pixels, pxcount * 2);  // RGB565 = 2 bytes/px
}

void St77916Panel::fillColor(uint16_t rgb565) {
    // Small stack buffer repeatedly blasted to the panel. 64 px × 2 bytes
    // = 128 bytes per transaction, multiplied into a full frame.
    constexpr size_t kChunkPx = 64;
    uint16_t buf[kChunkPx];
    for (size_t i = 0; i < kChunkPx; ++i) buf[i] = rgb565;

    const uint8_t caset[4] = { 0, 0, static_cast<uint8_t>((PANEL_WIDTH - 1) >> 8),
                               static_cast<uint8_t>((PANEL_WIDTH - 1) & 0xFF) };
    const uint8_t raset[4] = { 0, 0, static_cast<uint8_t>((PANEL_HEIGHT - 1) >> 8),
                               static_cast<uint8_t>((PANEL_HEIGHT - 1) & 0xFF) };
    sendCommand(0x2A, caset, 4);
    sendCommand(0x2B, raset, 4);

    size_t remaining = static_cast<size_t>(PANEL_WIDTH) * PANEL_HEIGHT;
    while (remaining > 0) {
        const size_t n = remaining > kChunkPx ? kChunkPx : remaining;
        sendPixels(buf, n * 2);
        remaining -= n;
    }
}

// ── Private helpers ───────────────────────────────────────────────────────

bool St77916Panel::initBus() {
    const spi_host_device_t host = static_cast<spi_host_device_t>(QSPI_HOST);

    // Deliberately NO spi_bus_free(host) here. Earlier bring-up added one
    // as a "paranoia cleanup" but it can leave the IDF's internal DMA
    // state in a half-released condition — a subsequent init returns
    // ESP_OK but with bus_attr->dma_enabled=false, which caps every
    // transfer at the 64-byte hardware FIFO. On ESP32-S3 with SPI3_HOST
    // there is no Arduino-side competition anyway; the bus should be
    // untouched at this point in boot.

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num     = QSPI_PIN_CLK;
    buscfg.data0_io_num    = QSPI_PIN_D0;
    buscfg.data1_io_num    = QSPI_PIN_D1;
    buscfg.data2_io_num    = QSPI_PIN_D2;
    buscfg.data3_io_num    = QSPI_PIN_D3;
    // Size this to cover one LVGL flush (40 lines × 480 px × 2 B ≈ 38 KB)
    // plus a little slack, not a full frame. DMA descriptor allocation
    // scales with this number; a full 480×480×2 B = 450 KB setting is
    // wasteful and in some IDF builds triggers silent allocation failures
    // that leave DMA disabled.
    buscfg.max_transfer_sz = PANEL_WIDTH * 40 * 2 + 64;
    buscfg.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;

    esp_err_t err = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
    log_i("[st77916] spi_bus_initialize rc=0x%x (%s)", err, esp_err_to_name(err));
    if (err != ESP_OK) {
        // INVALID_STATE after spi_bus_free means something re-claimed it
        // between our free and init — not recoverable here.
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits   = 8;
    devcfg.address_bits   = 24;
    devcfg.dummy_bits     = 0;
    devcfg.mode           = 0;
    devcfg.clock_speed_hz = QSPI_CLOCK_HZ;
    devcfg.spics_io_num   = QSPI_PIN_CS;
    devcfg.queue_size     = 7;
    devcfg.flags          = SPI_DEVICE_HALFDUPLEX;

    err = spi_bus_add_device(host, &devcfg, &asHandle(spi_dev_));
    log_i("[st77916] spi_bus_add_device rc=0x%x (%s)", err, esp_err_to_name(err));
    if (err != ESP_OK) return false;

    return true;
}

bool St77916Panel::resetPanel(Ch422g& io) {
    // Sequence: assert reset (EXIO2 low), hold, deassert, then wait out the
    // ST77916's post-reset quiet period before sending SWRESET.
    io.clearBits(CH422G_BIT_LCD_RST);
    delay(10);
    io.setBits(CH422G_BIT_LCD_RST);
    delay(120);
    return true;
}

bool St77916Panel::runInitSequence() {
    for (const auto& e : kInit) {
        if (e.paramCount == 0) {
            sendCommand(e.cmd);
        } else {
            sendCommand(e.cmd, e.params, e.paramCount);
        }
        if (e.delayMs) delay(e.delayMs);
    }
    return true;
}

void St77916Panel::sendCommand(uint8_t cmd) {
    sendCommand(cmd, nullptr, 0);
}

void St77916Panel::sendCommand(uint8_t cmd, const uint8_t* data, size_t len) {
    // ST77916 QSPI command frame:
    //   CMD byte  = 0x02 (write, command-on-single-line variant)
    //   ADDR bits = 0x00 << 16 | cmd << 8 | 0x00
    //   data      = len bytes, clocked on 4 lines (quad)
    spi_transaction_ext_t t = {};
    t.base.flags    = SPI_TRANS_MODE_QIO
                    | SPI_TRANS_VARIABLE_CMD
                    | SPI_TRANS_VARIABLE_ADDR
                    | SPI_TRANS_VARIABLE_DUMMY;
    t.base.cmd      = 0x02;
    t.base.addr     = (uint32_t)cmd << 8;
    t.command_bits  = 8;
    t.address_bits  = 24;
    t.dummy_bits    = 0;

    if (len > 0) {
        t.base.length = len * 8;
        t.base.tx_buffer = data;
    } else {
        t.base.length = 0;
        t.base.tx_buffer = nullptr;
    }
    spi_device_polling_transmit(asHandle(spi_dev_), &t.base);
}

void St77916Panel::sendPixels(const void* pixels, size_t bytes) {
    // ST77916 QSPI pixel frame — command+address+data all in quad mode.
    //
    // We cap each transaction at SOC_SPI_MAXIMUM_BUFFER_SIZE (64 bytes on
    // ESP32-S3) and loop. That's the size of the SPI hardware FIFO; any
    // transaction ≤ that always works whether DMA is engaged or not. If
    // bring-up later confirms DMA is actually on (no more "txdata transfer
    // > hardware max supported len" logs and the first full refresh shows
    // pixels), we can lift this cap for a big speed-up — DMA can do tens
    // of KB per transaction. Until then, correctness > speed.
    //
    // On the ST77916, subsequent MEMWRITE-continue calls simply append to
    // the pixel stream at the current GRAM write pointer, so splitting a
    // single block across multiple transactions is safe — we still send
    // the "start pixel frame" command (0x32 / 0x2C) each time because the
    // controller accepts it as "continue writing". (This matches how
    // Waveshare's own driver blasts line-buffers chunk-by-chunk.)
    constexpr size_t kMaxChunk = 64;  // == SOC_SPI_MAXIMUM_BUFFER_SIZE on S3
    const uint8_t* p = static_cast<const uint8_t*>(pixels);
    while (bytes > 0) {
        const size_t n = bytes > kMaxChunk ? kMaxChunk : bytes;
        spi_transaction_ext_t t = {};
        t.base.flags    = SPI_TRANS_MODE_QIO
                        | SPI_TRANS_MODE_DIOQIO_ADDR
                        | SPI_TRANS_VARIABLE_CMD
                        | SPI_TRANS_VARIABLE_ADDR
                        | SPI_TRANS_VARIABLE_DUMMY;
        t.base.cmd      = 0x32;
        t.base.addr     = (uint32_t)0x2C << 8;
        t.command_bits  = 8;
        t.address_bits  = 24;
        t.dummy_bits    = 0;
        t.base.length   = n * 8;
        t.base.tx_buffer = p;
        spi_device_polling_transmit(asHandle(spi_dev_), &t.base);
        p     += n;
        bytes -= n;
    }
}

}  // namespace display
