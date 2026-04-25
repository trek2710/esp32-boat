// st7701_panel.cpp — ST7701 RGB-parallel panel driver implementation.
// See st7701_panel.h for the rationale; see display_pins.h for pins.

#include "st7701_panel.h"

#include <Arduino.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "display_pins.h"
#include "tca9554.h"

namespace display {
namespace {

// --- Round 35: vsync gating -----------------------------------------------
//
// IDF 4.4.7's esp_lcd_panel_rgb keeps a single PSRAM framebuffer that the
// RGB peripheral DMA-scans continuously at ~58 Hz. esp_lcd_panel_draw_bitmap
// memcpy's into that same framebuffer, with no intrinsic synchronisation
// against the ongoing scan. Without gating, every flush from LVGL races the
// beam and the panel renders a mix of old + new rows for one or two frames
// each — that's the side-to-side shimmer on the dial labels in IMG_1907.MOV.
//
// The fix is to register cfg.on_frame_trans_done. In IDF 4.4 the RGB driver
// invokes this callback at the moment one full frame has finished shifting
// out, which is the start of the next vertical blanking interval — the only
// moment the framebuffer is safe to start rewriting and still let the
// memcpy stay ahead of the next row-0 scan.
//
// The callback runs in ISR context on core 1 (where the RGB peripheral
// driver lives), so we keep its body tiny: give a binary semaphore with
// FromISR. Ui's flush_cb takes that semaphore via St7701Panel::waitForVsync
// before calling drawBitmap.
SemaphoreHandle_t g_vsync_sem = nullptr;

bool IRAM_ATTR onFrameTransDone(esp_lcd_panel_handle_t /*panel*/,
                                esp_lcd_rgb_panel_event_data_t* /*ed*/,
                                void* /*user_ctx*/) {
    if (g_vsync_sem == nullptr) return false;
    BaseType_t hp_woken = pdFALSE;
    xSemaphoreGiveFromISR(g_vsync_sem, &hp_woken);
    return hp_woken == pdTRUE;
}

// --- 3-wire SPI init bus ---------------------------------------------------
//
// CS is on TCA9554 IO2 (Waveshare EXIO3; round 14 had this labelled IO3
// because of the off-by-one EXIO-naming misread, corrected in round 15).
// SCK and SDA are bit-banged on two spare GPIOs that
// are not otherwise claimed on this board — GPIO1 and GPIO2 both idle HIGH
// in the round-13 boot scan, and neither is part of the 16 parallel RGB
// data lines, so reusing them for one-shot init traffic won't fight the
// later RGB bus.
//
// The ST7701 sampling spec is quite forgiving (SCK min period ~66 ns) but
// we're sending through the expander at I2C-speed CS toggles anyway, so
// we just step at a conservative ~10 µs / bit and call it a day.
constexpr int kInitSpiSck = 2;
constexpr int kInitSpiSda = 1;

inline void spiBitDelay() {
    // A single delayMicroseconds(1) is plenty — digitalWrite itself is
    // ~500 ns on an S3 at 240 MHz, so the total bit period is well above
    // the ST7701 minimum.
    delayMicroseconds(1);
}

// ST7701 vendor-specific init sequence. Sourced from
// Nicolai-Electronics/esp32-component-st7701 and
// espressif/esp-iot-solution's esp_lcd_st7701 — both of which derive
// identical byte-for-byte content from the ST7701S datasheet's
// recommended power-up sequence + the panel-specific gamma/GIP values
// tuned for a standard 480×480 IPS cell. This matches what the Waveshare
// factory image ships with; if we ever see colour or gamma oddities we
// can tweak the B0/B1 (BK0 gamma) and E5/E8 (BK1 GIP timing) blocks.
//
// Entries are { command_byte, params..., param_count, post_delay_ms }.
// param_count == 0 means "command with no parameters" (so the 0x00 in the
// params array is just a placeholder the transport ignores).
struct St7701InitCmd {
    uint8_t       cmd;
    uint8_t       params[16];
    uint8_t       param_count;
    uint16_t      delay_ms;
};

constexpr uint8_t kCmdSlpout = 0x11;
constexpr uint8_t kCmdDispon = 0x29;

// ---------------------------------------------------------------------------
// Round 30 — init sequence replaced VERBATIM with the authoritative
// Waveshare config from esp-arduino-libs/ESP32_Display_Panel, file
// `src/board/supported/waveshare/BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1.h`,
// macro `ESP_PANEL_BOARD_LCD_VENDOR_INIT_CMD()`.
//
// That header is the library's Waveshare-board profile and was
// cross-validated against our `display_pins.h`: data pins, sync pins,
// I2C pins, IO-expander address all match byte-for-byte (data pins
// 5/45/48/47/21/14/13/12/11/10/9/46/3/8/18/17, sync 38/39/40/41, I2C
// SDA=15/SCL=7, TCA9554 at 0x20). Pair that with the reference's
// timings (HPW 8 / HBP 10 / HFP 50 / VPW 3 / VBP 8 / VFP 8, PCLK
// 16 MHz) applied in initRgbPanel() this round, and the round-30
// config IS Waveshare's shipped config for our exact board variant.
//
// Systemic deltas from round 29's init (all being corrected this
// round rather than tuned):
//
//   (1) BK3 ordering. Round 19 opened the init with BK3 before BK0.
//       Waveshare's sequence runs BK3 AFTER BK1/BK0, just before
//       leaving CMD2 mode. Ordering matters because the 0xEF in BK3
//       latches a source-driver bias that references the BK1 GIP
//       state; sending BK3's 0xEF before BK1 is set up means the
//       bias is taken against the panel's reset defaults, not our
//       configured GIP.
//
//   (2) Gamma polynomials (0xB0/0xB1 in BK0). Round 19 used
//       espressif's generic 480×480-ST7701 polynomials, which are
//       tuned for espressif's reference panel module. Waveshare's
//       values are tuned for the actual IPS cell in our module.
//
//   (3) Power-rail trims (BK1 0xB0/B1/B2/B5/B8). Round 19's values
//       were espressif's; Waveshare's single-byte trims differ by
//       up to ~10 hex per register, which shifts VOP / VCOM / gate
//       voltages by several hundred mV — enough to change whether
//       the source drivers correctly pulse all 480 columns.
//
//   (4) 0xC1 / 0xC2 in BK0. Round 19 had {0x10, 0x02} and {0x20,
//       0x06}. Waveshare: {0x0B, 0x02} and {0x07, 0x02}. The 0xC2
//       discrepancy in particular was the subject of rounds 25/26
//       (we swapped between {0x20, 0x06} and {0x07, 0x0A}, the
//       latter turning out worse); neither value was the actually-
//       shipped one.
//
//   (5) GIP block 0xE0..0xED. This is the largest class of change.
//       Every byte of 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
//       0xE8, 0xEB, 0xED differs between round 19's espressif
//       values and Waveshare's shipped values. Round 29's final
//       note correctly predicted this would be the next lever if
//       COLMOD=0x66 didn't fix colour; round 30 addresses it in
//       the same pass.
//
//   (6) 0xCD (BK0) added. Round 19 had only 0xCC; Waveshare sends
//       a companion 0xCD = 0x08 right after 0xCC = 0x10. This pair
//       controls the gate-scan and source-output direction together.
//
//   (7) 0x36 MADCTL added (= 0x00, no row/col swap, BGR off).
//       Round 19 omitted this; Waveshare sends it explicitly after
//       leaving CMD2 so the panel's memory scan direction is
//       deterministic regardless of its reset default.
//
//   (8) SLPOUT post-delay 120 → 480 ms. ST7701 datasheet says ≥120;
//       Waveshare uses 480 for extra margin on this module's TFT.
//
//   (9) 0x20 INVOFF added between SLPOUT and DISPON (with its own
//       120 ms post-delay). Round 19 omitted this. The datasheet
//       default for INV state depends on sub-module strap; Waveshare
//       sends 0x20 (normal, not inverted) explicitly.
//
//  (10) 0x3A COLMOD kept at 0x66 (as round 29). Waveshare confirms
//       this: their `ESP_PANEL_BOARD_LCD_RGB_PIXEL_BITS` is defined
//       `RGB565` for the peripheral side, but the panel-side COLMOD
//       is `0x66` (18-bit). Round 29's isolated COLMOD change
//       regressed only because the rest of the init was wrong; the
//       0x66 setting itself was correct and stays.
// ---------------------------------------------------------------------------
const St7701InitCmd kInitCmds[] = {
    // Enter CMD2 BK0 — display size, voltages, inversion, gamma.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, {0x3B, 0x00}, 2, 0},
    {0xC1, {0x0B, 0x02}, 2, 0},
    // Round 33: 0xC2 byte 0 0x07 → 0x37 to attack the wide brightness
    // bands on solid fills (IMG_1895/1896/1897/1900). 0xC2 is the ST7701
    // Display Inversion Control register: byte 0 bits [5:4] select the
    // inversion scheme (00=column, 01=1-line, 10=1-dot, 11=column/alt),
    // byte 1 is the RTN timing. Waveshare ships {0x07, 0x02} = bits [5:4]
    // = 00 = column inversion, which is producing the bands we see.
    // Setting byte 0 to 0x37 (= 0x07 | 0x30) flips bits [5:4] to 11 —
    // the alternate inversion scheme that Espressif's generic ST7701
    // references (e.g. {0x37, 0x05}) use successfully on other 480x480
    // panels. Round 25 also touched 0xC2 but ONLY byte 1 (0x06 → 0x0A),
    // which leaves the inversion mode identical and only changes the
    // RTN timing — that's why round 25 didn't flatten banding and did
    // regress other symptoms. Round 33 keeps byte 1 at 0x02 (Waveshare-
    // stable RTN) and changes ONLY the mode bits for a clean signal.
    {0xC2, {0x37, 0x02}, 2, 0},
    {0xCC, {0x10}, 1, 0},
    {0xCD, {0x08}, 1, 0},
    // Positive-polarity gamma (BK0 0xB0).
    {0xB0, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x06, 0x05, 0x09,
            0x08, 0x21, 0x06, 0x13, 0x10, 0x29, 0x31, 0x18}, 16, 0},
    // Negative-polarity gamma (BK0 0xB1).
    {0xB1, {0x00, 0x11, 0x16, 0x0E, 0x11, 0x07, 0x05, 0x09,
            0x09, 0x21, 0x05, 0x13, 0x11, 0x2A, 0x31, 0x18}, 16, 0},

    // Enter CMD2 BK1 — power-rail trims, VCOM, GIP.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, {0x6D}, 1, 0},        // VOP amplitude
    {0xB1, {0x37}, 1, 0},        // VCOM amplitude
    {0xB2, {0x81}, 1, 0},
    {0xB3, {0x80}, 1, 0},
    {0xB5, {0x43}, 1, 0},
    {0xB7, {0x85}, 1, 0},
    {0xB8, {0x20}, 1, 0},
    {0xC1, {0x78}, 1, 0},
    {0xC2, {0x78}, 1, 0},
    {0xD0, {0x88}, 1, 0},        // (Waveshare has no post-delay here; round 19 had 100 ms)

    // GIP (Gate-In-Panel). Waveshare-shipped values; every byte
    // differs from the espressif generic reference we used in round 19.
    {0xE0, {0x00, 0x00, 0x02}, 3, 0},
    {0xE1, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00,
            0x00, 0x20, 0x20}, 11, 0},
    {0xE2, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00}, 13, 0},
    {0xE3, {0x00, 0x00, 0x11, 0x00}, 4, 0},
    {0xE4, {0x22, 0x00}, 2, 0},
    {0xE5, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 16, 0},
    {0xE6, {0x00, 0x00, 0x11, 0x00}, 4, 0},
    {0xE7, {0x22, 0x00}, 2, 0},
    {0xE8, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 16, 0},
    {0xEB, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}, 7, 0},
    {0xED, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF,
            0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}, 16, 0},
    {0xEF, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}, 6, 0},

    // Enter CMD2 BK3 AFTER BK1 (Waveshare ordering, not espressif's).
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, {0x08}, 1, 0},

    // Leave CMD2 mode.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    // Memory access / pixel format.
    //
    // Round 32: MADCTL 0xC0 → 0x00 (revert to Waveshare default).
    // Round 31 set MADCTL to 0xC0 (MY+MX = 180°) to correct the
    // upside-down rendering seen in round 30's IMG_1888. The reflash
    // in round 31 (IMG_1894) showed IDENTICAL orientation — HDG/COG
    // still at top-of-photo, mirror-reversed — meaning MADCTL had no
    // visible effect on the displayed image. Reason: on ST7701 driven
    // through the 16-bit RGB parallel interface, host pixels stream
    // directly to the source drivers and bypass internal GRAM; the
    // MADCTL bits (MY/MX/MV/ML) only re-order GRAM access, so they
    // don't rotate the displayed frame. Rotation therefore has to be
    // applied on the host side. Round 32 reverts MADCTL to the factory
    // default and implements the 180° flip in Ui.cpp::flushCb instead
    // (reverse pixel buffer + flip draw rectangle).
    {0x36, {0x00}, 1, 0},        // MADCTL: factory default (rotation done in flushCb)
    {0x3A, {0x66}, 1, 0},        // COLMOD: 18bpp RGB666 on the RGB interface

    // Sleep out + 480 ms stabilisation.
    {kCmdSlpout, {0x00}, 0, 480},
    // Inversion off + 120 ms.
    {0x20, {0x00}, 0, 120},
    // Display on (no post-delay per Waveshare).
    {kCmdDispon, {0x00}, 0, 0},
};

}  // namespace

// ---------------------------------------------------------------------------
// Panel reset via TCA9554. LCD_RST is active-low on IO0 (= EXIO1 in
// Waveshare's 1-indexed helper naming; round 14 had this on IO1, which is
// actually TP_RST — see the round-15 note in display_pins.h). Hold RST low
// for 20 ms, release for 120 ms. The ST7701S datasheet specifies ≥10 µs for
// the reset pulse width and ≥120 ms before the first init command, so our
// margins are comfortable.
// ---------------------------------------------------------------------------
bool St7701Panel::resetPanel(Tca9554& io) {
    // Safe default: CS deasserted (high), RST asserted (low), BL off.
    // The CS bit is held high so the panel ignores whatever GPIO noise
    // SCK/SDA produce while we're setting up output mode on them.
    uint8_t state = TCA9554_BIT_LCD_CS;   // CS=1, everything else 0
    if (!io.writeOutput(state)) {
        log_e("[st7701] TCA9554 pre-reset write failed");
        return false;
    }
    delay(20);

    // Release reset.
    state |= TCA9554_BIT_LCD_RST;
    if (!io.writeOutput(state)) {
        log_e("[st7701] TCA9554 reset-release write failed");
        return false;
    }
    delay(120);
    return true;
}

// ---------------------------------------------------------------------------
// 3-wire SPI bit-bang. ST7701 samples SDA on the rising edge of SCK. The
// DCX bit (first of 9) selects command (0) vs data (1); the following 8
// bits are the value, MSB first. Caller must CS-assert / CS-deassert
// around each byte — we do that inside this function for convenience.
// ---------------------------------------------------------------------------
void St7701Panel::spiWriteByte(Tca9554& io, bool is_data, uint8_t value) {
    // Idle state: SCK low, SDA don't-care, CS high.
    digitalWrite(kInitSpiSck, LOW);

    // CS low = select.
    io.clearBits(TCA9554_BIT_LCD_CS);

    // Bit 8: DCX.
    digitalWrite(kInitSpiSda, is_data ? HIGH : LOW);
    spiBitDelay();
    digitalWrite(kInitSpiSck, HIGH);
    spiBitDelay();
    digitalWrite(kInitSpiSck, LOW);

    // Bits 7..0: the data byte, MSB first.
    for (int b = 7; b >= 0; --b) {
        digitalWrite(kInitSpiSda, (value >> b) & 1 ? HIGH : LOW);
        spiBitDelay();
        digitalWrite(kInitSpiSck, HIGH);
        spiBitDelay();
        digitalWrite(kInitSpiSck, LOW);
    }

    // CS high = deselect — end of transaction.
    io.setBits(TCA9554_BIT_LCD_CS);
}

// ---------------------------------------------------------------------------
// Walk the init table, sending each command (DCX=0) followed by its
// parameters (DCX=1) and honouring per-command post-delays.
// ---------------------------------------------------------------------------
bool St7701Panel::runInitSequence(Tca9554& io) {
    pinMode(kInitSpiSck, OUTPUT);
    pinMode(kInitSpiSda, OUTPUT);
    digitalWrite(kInitSpiSck, LOW);
    digitalWrite(kInitSpiSda, LOW);

    const size_t n = sizeof(kInitCmds) / sizeof(kInitCmds[0]);
    for (size_t i = 0; i < n; ++i) {
        const auto& e = kInitCmds[i];
        spiWriteByte(io, /*is_data=*/false, e.cmd);
        for (uint8_t p = 0; p < e.param_count; ++p) {
            spiWriteByte(io, /*is_data=*/true, e.params[p]);
        }
        if (e.delay_ms) {
            delay(e.delay_ms);
        }
    }

    // Release SCK/SDA back to a safe idle. We don't reconfigure them as
    // inputs because esp_lcd_panel_rgb doesn't touch GPIO1/GPIO2 — they
    // just stay as outputs at whatever we last wrote, which is fine.
    digitalWrite(kInitSpiSck, LOW);
    digitalWrite(kInitSpiSda, LOW);

    log_i("[st7701] init sequence sent (%u commands)",
          static_cast<unsigned>(n));
    return true;
}

// ---------------------------------------------------------------------------
// Configure the ESP-IDF RGB-panel peripheral. This is where the 16 data
// lines, sync signals, and PCLK get routed and the framebuffer gets
// allocated in PSRAM.
// ---------------------------------------------------------------------------
bool St7701Panel::initRgbPanel() {
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.data_width      = 16;
    cfg.psram_trans_align = 64;
    cfg.clk_src         = LCD_CLK_SRC_PLL160M;

    cfg.timings.pclk_hz           = RGB_PCLK_HZ;
    cfg.timings.h_res             = PANEL_WIDTH;
    cfg.timings.v_res             = PANEL_HEIGHT;

    // Porch / pulse widths — Round 30.
    //
    // HPW 8 / HBP 10 / HFP 50 / VFP 8 already matched the authoritative
    // Waveshare config (`BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1.h` in
    // esp-arduino-libs/ESP32_Display_Panel, which maps our pins exactly
    // and is the library Waveshare themselves link from the product
    // wiki). Two deltas remain: VPW was 2 in round 28, Waveshare uses 3;
    // VBP was 18 (FatihErtugral's sibling-2.8" value), Waveshare uses 8.
    // Bringing both to the Waveshare values gives a total vertical
    // blanking of 3+8+8 = 19 lines → 499 lines/frame; horizontal
    // 480+8+10+50 = 548 PCLKs/line; 499 × 548 = 273,452 PCLKs/frame
    // @ 16 MHz → ~58.5 Hz refresh. This matches the combination shipped
    // in Waveshare's own demo code, so if it doesn't yield coherent
    // full-screen fills, the remaining degree of freedom is the init
    // sequence (which round 30 also replaces verbatim, see kInitCmds
    // above).
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch  = 10;
    cfg.timings.hsync_front_porch = 50;
    cfg.timings.vsync_pulse_width = 3;
    cfg.timings.vsync_back_porch  = 8;
    cfg.timings.vsync_front_porch = 8;

    // Polarity flags (round 16 fix).
    //
    // Round 15 had pclk_active_neg = 1, which tells the RGB peripheral to
    // drive data on the falling edge of PCLK. That caused the round-15
    // symptom: the panel came alive with colored vertical stripes
    // compressed into the middle ~50% of the screen — classic
    // "data sampled on the wrong PCLK edge" pattern, every other pixel
    // gets captured with stale data so columns smear together into stripes.
    //
    // FatihErtugral's working config for the sibling 2.8" Waveshare ST7701
    // board uses pclk_active_neg = 0, meaning data is valid on the rising
    // edge of PCLK. That's the standard ST7701 RGB-mode sampling and what
    // the ST7701S datasheet shows in its RGB interface timing diagram.
    //
    // The other three flags (hsync_idle_low, vsync_idle_low, de_idle_high)
    // all stay at 0 — HSYNC/VSYNC idle HIGH and pulse LOW during sync
    // (active-low sync pulses, which is the ST7701 default), DE is active-
    // high so it idles low.
    cfg.timings.flags.hsync_idle_low  = 0;
    cfg.timings.flags.vsync_idle_low  = 0;
    cfg.timings.flags.de_idle_high    = 0;
    cfg.timings.flags.pclk_active_neg = 0;   // <-- round 16: was 1
    cfg.timings.flags.pclk_idle_high  = 0;

    cfg.hsync_gpio_num = RGB_PIN_HSYNC;
    cfg.vsync_gpio_num = RGB_PIN_VSYNC;
    cfg.de_gpio_num    = RGB_PIN_DE;
    cfg.pclk_gpio_num  = RGB_PIN_PCLK;
    // Byte order on this board: blue lines are the low byte, red lines
    // are the high byte — matches LVGL's LV_COLOR_16_SWAP=1 wire layout.
    cfg.data_gpio_nums[0]  = RGB_PIN_B0;
    cfg.data_gpio_nums[1]  = RGB_PIN_B1;
    cfg.data_gpio_nums[2]  = RGB_PIN_B2;
    cfg.data_gpio_nums[3]  = RGB_PIN_B3;
    cfg.data_gpio_nums[4]  = RGB_PIN_B4;
    cfg.data_gpio_nums[5]  = RGB_PIN_G0;
    cfg.data_gpio_nums[6]  = RGB_PIN_G1;
    cfg.data_gpio_nums[7]  = RGB_PIN_G2;
    cfg.data_gpio_nums[8]  = RGB_PIN_G3;
    cfg.data_gpio_nums[9]  = RGB_PIN_G4;
    cfg.data_gpio_nums[10] = RGB_PIN_G5;
    cfg.data_gpio_nums[11] = RGB_PIN_R0;
    cfg.data_gpio_nums[12] = RGB_PIN_R1;
    cfg.data_gpio_nums[13] = RGB_PIN_R2;
    cfg.data_gpio_nums[14] = RGB_PIN_R3;
    cfg.data_gpio_nums[15] = RGB_PIN_R4;

    cfg.disp_gpio_num  = -1;  // DISP / backlight handled via TCA9554, not a GPIO
    // Round 35: hook the frame-trans-done callback so flushCb can gate its
    // memcpy on vsync. Without this, esp_lcd_panel_draw_bitmap and the
    // continuous RGB DMA scan race within the single shared PSRAM
    // framebuffer (IDF 4.4.7 only allocates one) and the panel mixes old +
    // new rows for ~1 frame per flush. See onFrameTransDone above.
    if (g_vsync_sem == nullptr) {
        g_vsync_sem = xSemaphoreCreateBinary();
        if (g_vsync_sem == nullptr) {
            log_e("[st7701] xSemaphoreCreateBinary failed - falling back to "
                  "unsynchronised flushes (will tear like rounds 32-34)");
        }
    }
    cfg.on_frame_trans_done = onFrameTransDone;
    cfg.user_ctx       = nullptr;

    // Framebuffer: one full-screen RGB565 buffer in PSRAM. 480×480×2 =
    // 460,800 bytes, well under our 8 MB budget.
    //
    // Round 42 attempted to enable bounce-buffer mode here (
    // cfg.bounce_buffer_size_px / cfg.flags.bb_invalidate_cache) — it
    // would have decoupled PSRAM framebuffer writes from the RGB scan,
    // killing the residual shimmer round 35's vsync gate can't catch on
    // big LVGL redraws. But the Arduino-ESP32 2.0.16 IDF fork's
    // esp_lcd_rgb_panel_config_t doesn't include those fields (they
    // landed in mainline IDF 4.3, but this Arduino-ESP32 release pins
    // an older snapshot). Compile failure: "no member named
    // bounce_buffer_size_px". Reverted; mitigation moved to Ui.cpp
    // (dedup'ed indicator updates + slower visible-page refresh).
    cfg.flags.fb_in_psram       = 1;
    cfg.flags.disp_active_low   = 0;
    cfg.flags.relax_on_idle     = 0;

    esp_lcd_panel_handle_t handle = nullptr;
    esp_err_t rc = esp_lcd_new_rgb_panel(&cfg, &handle);
    if (rc != ESP_OK) {
        log_e("[st7701] esp_lcd_new_rgb_panel rc=0x%x (%s)",
              rc, esp_err_to_name(rc));
        return false;
    }

    rc = esp_lcd_panel_reset(handle);
    if (rc != ESP_OK) {
        log_e("[st7701] esp_lcd_panel_reset rc=0x%x (%s)",
              rc, esp_err_to_name(rc));
        return false;
    }
    rc = esp_lcd_panel_init(handle);
    if (rc != ESP_OK) {
        log_e("[st7701] esp_lcd_panel_init rc=0x%x (%s)",
              rc, esp_err_to_name(rc));
        return false;
    }

    panel_ = handle;
    log_i("[st7701] RGB panel up (%d×%d @ %d Hz PCLK, fb in PSRAM)",
          PANEL_WIDTH, PANEL_HEIGHT, RGB_PCLK_HZ);
    return true;
}

// ---------------------------------------------------------------------------
// Full bring-up — reset → init SPI → RGB panel → backlight on.
// ---------------------------------------------------------------------------
bool St7701Panel::begin(Tca9554& io) {
    log_i("[st7701] resetPanel");
    if (!resetPanel(io)) return false;

    log_i("[st7701] runInitSequence (3-wire SPI @ SCK=GPIO%d SDA=GPIO%d, "
          "CS=TCA9554 IO2 / EXIO3)", kInitSpiSck, kInitSpiSda);
    if (!runInitSequence(io)) return false;

    log_i("[st7701] initRgbPanel");
    if (!initRgbPanel()) return false;

    // Backlight is on RAW GPIO6 with PWM, not on the TCA9554 (round 15 fix).
    // Round 14 was writing to TCA9554 IO0 which is actually LCD_RST (off-by-
    // one EXIO-naming misread) — that's both why the panel never came out of
    // reset properly AND why the screen stayed dark: LCD_RST got stuck HIGH-
    // then-low-then-stuck-high again, and nothing was driving GPIO6, so the
    // backlight FET's gate sat at 0 V and the LED array never lit.
    log_i("[st7701] backlight: ledcSetup(ch=%d, freq=%d Hz, res=%d-bit), "
          "ledcAttachPin(%d, %d), ledcWrite(%d, %d) [full brightness]",
          BACKLIGHT_PWM_CH, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_RES,
          BACKLIGHT_PIN, BACKLIGHT_PWM_CH,
          BACKLIGHT_PWM_CH, BACKLIGHT_PWM_FULL);
    ledcSetup(BACKLIGHT_PWM_CH, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_RES);
    ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_PWM_CH);
    ledcWrite(BACKLIGHT_PWM_CH, BACKLIGHT_PWM_FULL);

    ready_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// drawBitmap — forward to esp_lcd_panel_draw_bitmap. The coordinate
// convention mirrors LVGL's: (x1,y1)-(x2,y2) inclusive, so we pass
// x_end = x2+1, y_end = y2+1 as esp_lcd expects exclusive end.
// ---------------------------------------------------------------------------
void St7701Panel::drawBitmap(int x1, int y1, int x2, int y2,
                             const void* pixels) {
    if (!ready_ || !panel_) return;
    auto handle = static_cast<esp_lcd_panel_handle_t>(panel_);
    esp_lcd_panel_draw_bitmap(handle, x1, y1, x2 + 1, y2 + 1, pixels);
}

// Round 35: take the binary semaphore that onFrameTransDone gives at
// vblank. 50 ms timeout = ~3 frames at 58 Hz; if the callback ever fails
// to fire (e.g. a hypothetical IDF bug or a dead PSRAM bus stall) we
// release the caller after that bound rather than wedging the UI task.
// At the moment this returns, the RGB peripheral has just finished
// shifting the previous frame and is sitting in vertical blanking — the
// safest possible point to start rewriting the framebuffer.
void St7701Panel::waitForVsync() {
    if (g_vsync_sem == nullptr) return;
    xSemaphoreTake(g_vsync_sem, pdMS_TO_TICKS(50));
}

void St7701Panel::fillColor(uint16_t rgb565) {
    if (!ready_ || !panel_) return;
    //
    // Round-24 rewrite. The earlier version built a single 960-byte row
    // in PSRAM and issued 480 back-to-back esp_lcd_panel_draw_bitmap()
    // calls, one per scanline. That seemed innocent and kept peak
    // memory low, but the round-23 colour-bar photos told a different
    // story: every phase left the middle third of the screen showing
    // the PREVIOUS phase's colour (IMG_1815 RED phase showed blue
    // stripes in the middle, IMG_1816 GREEN showed blue, IMG_1817 BLUE
    // showed GREEN, IMG_1818 WHITE showed darker-blue, IMG_1819 BLACK
    // showed greenish-dark). Outer columns were always correct, middle
    // third always stale. That pattern is the fingerprint of an
    // IDF 4.4.7-era PSRAM-framebuffer coherency hole: the RGB peripheral
    // reads the PSRAM framebuffer in fixed-size DMA bursts, the CPU
    // writes rows through the cache, and with 480 small writes the
    // cache-to-DMA sync can drop the middle of each line (the bursts
    // that straddle the 160-column and 320-column boundaries).
    //
    // Fix: allocate one full-screen 480×480×2 = 460 800-byte PSRAM
    // buffer, fill it with the solid colour, and call
    // esp_lcd_panel_draw_bitmap() exactly ONCE for the entire screen.
    // IDF's internal memcpy-to-framebuffer then runs as a single
    // contiguous operation and the cache/DMA sync happens once at
    // known-good boundaries. 460 KB off PSRAM is cheap: we have 8 MB
    // and the peripheral's own framebuffer is the same size, so peak
    // usage during fillColor is ~920 KB + LVGL's ~77 KB double-buffer
    // — under 1.1 MB total, comfortably inside budget.
    //
    // The buffer is freed immediately after the draw call; this is not
    // a hot path (five calls total, only at bring-up), so the alloc
    // churn does not matter.
    //
    const size_t total_px    = static_cast<size_t>(PANEL_WIDTH) * PANEL_HEIGHT;
    const size_t total_bytes = total_px * sizeof(uint16_t);
    uint16_t* fb = static_cast<uint16_t*>(
        heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!fb) {
        log_e("[st7701] fillColor: PSRAM alloc failed (%u bytes)",
              static_cast<unsigned>(total_bytes));
        return;
    }
    for (size_t i = 0; i < total_px; ++i) fb[i] = rgb565;
    drawBitmap(0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1, fb);
    heap_caps_free(fb);
}

}  // namespace display
