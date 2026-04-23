// st7701_panel.cpp — ST7701 RGB-parallel panel driver implementation.
// See st7701_panel.h for the rationale; see display_pins.h for pins.

#include "st7701_panel.h"

#include <Arduino.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#include "display_pins.h"
#include "tca9554.h"

namespace display {
namespace {

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

constexpr uint8_t kCmdNoron  = 0x13;
constexpr uint8_t kCmdSlpout = 0x11;
constexpr uint8_t kCmdDispon = 0x29;

const St7701InitCmd kInitCmds[] = {
    // Select Command2 BK3 — the "enter vendor command mode" prelude.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {kCmdNoron, {0x00}, 0, 0},
    {0xEF, {0x08}, 1, 0},

    // Command2 BK0: resolution, scan direction, frame rate, gamma.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    // 0xC0 "Display Line Setting": byte 1 = NL[7:0], where number of
    // display lines = (NL + 1) × 8. For our 480-line panel, NL = 59 =
    // 0x3B → (59+1)×8 = 480 lines. Round 13–16 had this as 0x63 (= 99)
    // which configures the panel for (99+1)×8 = 800 lines — not a value
    // this physical cell can drive correctly. That mis-programming is
    // a strong candidate for round 16's "vertical colored stripes in
    // middle ~50%" symptom: feeding 480 lines of RGB data into a panel
    // internally gated for 800 lines would smear rows across the array
    // in exactly the sort of pattern we're seeing. Fixed in round 17.
    {0xC0, {0x3B, 0x00}, 2, 0},  // display line = 480  (NL=59 → (59+1)*8)
    {0xC1, {0x10, 0x02}, 2, 0},  // VBP / VFP
    {0xC2, {0x37, 0x08}, 2, 0},  // inversion type
    {0xCC, {0x38}, 1, 0},
    {0xB0, {0x40, 0xC9, 0x90, 0x0D, 0x0F, 0x04, 0x00, 0x07,
            0x07, 0x1C, 0x04, 0x52, 0x0F, 0xDF, 0x26, 0xCF}, 16, 0},
    {0xB1, {0x40, 0xC9, 0xCF, 0x0C, 0x90, 0x04, 0x00, 0x07,
            0x08, 0x1B, 0x06, 0x55, 0x13, 0x62, 0xE7, 0xCF}, 16, 0},

    // Command2 BK1: power, voltages, GIP timing.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, {0x5D}, 1, 0},  // VOP amplitude
    {0xB1, {0x2D}, 1, 0},  // VCOM amplitude
    {0xB2, {0x07}, 1, 0},
    {0xB3, {0x80}, 1, 0},
    {0xB5, {0x08}, 1, 0},
    {0xB7, {0x85}, 1, 0},
    {0xB8, {0x20}, 1, 0},
    {0xB9, {0x10}, 1, 0},
    {0xC1, {0x78}, 1, 0},
    {0xC2, {0x78}, 1, 0},
    {0xD0, {0x88}, 1, 100},  // +100 ms before the big GIP block

    // GIP sequence (panel-specific timing for the gate drivers).
    {0xE0, {0x00, 0x19, 0x02}, 3, 0},
    {0xE1, {0x05, 0xA0, 0x07, 0xA0, 0x04, 0xA0, 0x06, 0xA0,
            0x00, 0x44, 0x44}, 11, 0},
    {0xE2, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00}, 13, 0},
    {0xE3, {0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, {0x44, 0x44}, 2, 0},
    {0xE5, {0x0D, 0x31, 0xC8, 0xAF, 0x0F, 0x33, 0xC8, 0xAF,
            0x09, 0x2D, 0xC8, 0xAF, 0x0B, 0x2F, 0xC8, 0xAF}, 16, 0},
    {0xE6, {0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, {0x44, 0x44}, 2, 0},
    {0xE8, {0x0C, 0x30, 0xC8, 0xAF, 0x0E, 0x32, 0xC8, 0xAF,
            0x08, 0x2C, 0xC8, 0xAF, 0x0A, 0x2E, 0xC8, 0xAF}, 16, 0},
    {0xEB, {0x02, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x40}, 7, 0},
    {0xEC, {0x3C, 0x00}, 2, 0},
    {0xED, {0xAB, 0x89, 0x76, 0x54, 0x01, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x10, 0x45, 0x67, 0x98, 0xBA}, 16, 0},

    // Leave vendor mode and turn the pixel pipeline on.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    // 0x3A "COLMOD / Interface Pixel Format" — round-17 added it, round-18
    // corrected the parameter from 0x66 to 0x55.
    //
    // Rounds 13–16 omitted this command entirely. Round 17 added it with
    // 0x66 (18-bit/pixel RGB666) because that's the default value in the
    // ST7701S datasheet and what the espressif esp-iot-solution ST7701
    // driver uses — but espressif's reference board wires 18 data lines,
    // not 16, so 0x66 matches THEIR hardware, not ours.
    //
    // On our 16-line RGB bus, COLMOD=0x66 tells the panel to treat the
    // incoming data as 18-bit RGB666 — bit DB[17..12] = R[5..0],
    // DB[11..6] = G[5..0], DB[5..0] = B[5..0]. But we only drive
    // DB[15..0]. The panel floats/zeros DB[17..16] AND re-interprets
    // every remaining bit against the 18bpp layout, so:
    //   our bit 15 (pixel R4, MSB of red)  → panel reads as R3 (bit shift)
    //   our bit 11 (pixel R0, LSB of red)  → panel reads as G5
    //   our bit 10 (pixel G5, MSB of green) → panel reads as G4
    //   ...
    // Result: every colour channel's MSB/LSB gets shifted by a bit or
    // two, and the "cross-talk" between channels produces exactly the
    // vertical-stripe colored pattern rounds 16/17 showed (symptom
    // shifted from "mostly blue" in round 16 to "more green+blue" in
    // round 17 because adding the 0x66 COLMOD changed WHICH bits
    // misalign, not whether they do).
    //
    // 0x55 = 16-bit/pixel RGB565 in BOTH RGB (DPI) and MCU (DBI)
    // interfaces. This matches our 16 data lines exactly:
    //   DB[15..11] = R[4..0]   DB[10..5] = G[5..0]   DB[4..0] = B[4..0]
    // which is the RGB565 layout the ESP32 RGB peripheral puts on the
    // wire when LV_COLOR_DEPTH=16 + LV_COLOR_16_SWAP=1. The ST7701S
    // datasheet (section 10.2.19) lists 0x55 as a valid value; some
    // community drivers avoid it out of caution, but it's correct for a
    // 16-line bus and is what LovyanGFX ships for Waveshare's 16-line
    // ST7701 boards.
    {0x3A, {0x55}, 1, 0},

    {kCmdSlpout, {0x00}, 0, 120},  // sleep out — panel needs 120 ms
    {kCmdDispon, {0x00}, 0, 50},   // display on
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

    // Porch / pulse widths. Values taken from FatihErtugral's working
    // ESP-IDF driver for the Waveshare ESP32-S3-Touch-LCD-2.8 (sibling
    // ST7701 board — same panel controller, different physical size).
    // Our round 15 used vsync_pulse_width=8 which works on the 2.8" board
    // too, but vsync_pulse_width=2 is the exact value the reference ships
    // with, so we use that to stay on the known-good side of the fence.
    cfg.timings.hsync_pulse_width = 8;
    cfg.timings.hsync_back_porch  = 10;
    cfg.timings.hsync_front_porch = 50;
    cfg.timings.vsync_pulse_width = 2;
    cfg.timings.vsync_back_porch  = 18;
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
    cfg.on_frame_trans_done = nullptr;
    cfg.user_ctx       = nullptr;

    // Framebuffer: one full-screen RGB565 buffer in PSRAM. 480×480×2 =
    // 460,800 bytes, well under our 8 MB budget.
    //
    // Note on the missing bounce_buffer_size_px field: IDF 5.x added a
    // "bounce buffer" fast path that keeps a small SRAM staging buffer in
    // front of the PSRAM framebuffer to work around PSRAM bus stalls at
    // high PCLK. Arduino-ESP32 2.0.16 ships with IDF 4.4.7, which predates
    // that feature — the struct doesn't have the field, so we can't set
    // it here. At 14 MHz PCLK this is fine; if we ever push PCLK past
    // ~20 MHz and see tearing, the fix is to bump the core to IDF 5.x
    // (Arduino-ESP32 3.x) rather than to add back this line.
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

void St7701Panel::fillColor(uint16_t rgb565) {
    if (!ready_ || !panel_) return;
    // Build one row in PSRAM (PANEL_WIDTH × 2 bytes) and repeat it line by
    // line. Keeps stack use bounded and costs PANEL_HEIGHT draw calls —
    // only done once at bring-up, so the throughput hit is irrelevant.
    uint16_t* row = static_cast<uint16_t*>(
        heap_caps_malloc(PANEL_WIDTH * sizeof(uint16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!row) {
        log_e("[st7701] fillColor: PSRAM alloc failed");
        return;
    }
    for (int i = 0; i < PANEL_WIDTH; ++i) row[i] = rgb565;
    for (int y = 0; y < PANEL_HEIGHT; ++y) {
        drawBitmap(0, y, PANEL_WIDTH - 1, y, row);
    }
    heap_caps_free(row);
}

}  // namespace display
