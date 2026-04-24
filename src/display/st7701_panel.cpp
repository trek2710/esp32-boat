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

constexpr uint8_t kCmdSlpout = 0x11;
constexpr uint8_t kCmdDispon = 0x29;

// ---------------------------------------------------------------------------
// Round-19 ST7701 init sequence — ported verbatim from espressif's
// esp-iot-solution/components/display/lcd/esp_lcd_st7701/esp_lcd_st7701_rgb.c
// `vendor_specific_init_default[]` table (commit on master, 2026-04).
//
// Why the full port: rounds 13–18 used an init table stitched together from
// Nicolai-Electronics/esp32-component-st7701 and the generic ST7701S
// datasheet sequence. That sequence turns the panel on but the photo from
// round 18 shows a uniform blue background with a column of green+blue
// vertical stripes spanning the middle ~1/3 of the screen. That symptom is
// a textbook gate-in-panel (GIP) timing mismatch: most columns of the
// panel's source drivers never get the right gate pulse, so they stay in
// their power-on (blue) state, while a narrow column band actually scans
// the data we're sending.
//
// Cross-checking our init against espressif's surfaced three systemic
// problems beyond individual byte differences:
//
//   (1) Our init opened with `{0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0}`
//       then sent `{0x13, {}, 0, 0}`. We intended 0x13 as a Command2 BK3
//       selector but sent it as the standalone MIPI command NORON (Normal
//       Display Mode On). Espressif instead opens with
//       `{0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0}` — BK3 selected via
//       the 0xFF parameter — and then sends `{0xEF, {0x08}, 1, 0}` in BK3
//       (our 0xEF landed in normal mode, where the command has no effect).
//
//   (2) Our 0xE0..0xED GIP block had panel-specific timing values that
//       don't match this cell. The GIP block tells the panel's gate
//       drivers when to pulse each row; wrong values → rows that never
//       activate → columns stuck in reset → the exact mid-band stripe we
//       see. Espressif's values are the known-good ones for the 480×480
//       Waveshare ST7701 boards.
//
//   (3) Rounds 17/18 added a 0x3A COLMOD command (0x66 then 0x55) on the
//       theory that pixel-format was wrong. But espressif's init OMITS
//       0x3A entirely — the panel's power-on default pixel format is
//       correct for its hardware strapping, and explicitly setting it to
//       a non-default value silently misaligns bits on the RGB bus. Round
//       19 removes the 0x3A addition.
//
// Aside from the three above, the byte-for-byte differences between rounds
// 13–18 and espressif's reference are too many to list inline; rather than
// ship another "mostly correct" variant, round 19 replaces the entire
// table with a verbatim port. If the panel still doesn't paint after this,
// the suspect shifts from "software init" to "RGB bus / timing" and we
// pivot to pin-order and porch experiments.
// ---------------------------------------------------------------------------
const St7701InitCmd kInitCmds[] = {
    // Enter Command2 BK3 and send its one-byte reserved setting.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, {0x08}, 1, 0},

    // Enter Command2 BK0: display size, voltages, inversion, gamma.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, {0x3B, 0x00}, 2, 0},  // NL = 59 → 480 display lines, SCN = 0
    {0xC1, {0x10, 0x02}, 2, 0},  // VBP = 16, VFP = 2
    {0xC2, {0x20, 0x06}, 2, 0},  // inversion type / dot inversion
    {0xCC, {0x10}, 1, 0},        // gate scan direction / source swap
    // Positive & negative gamma (BK0 0xB0 / 0xB1) — panel-specific
    // polynomial coefficients for the on-panel voltage→gray-level mapping.
    {0xB0, {0x00, 0x13, 0x5A, 0x0F, 0x12, 0x07, 0x09, 0x08,
            0x08, 0x24, 0x07, 0x13, 0x12, 0x6B, 0x73, 0xFF}, 16, 0},
    {0xB1, {0x00, 0x13, 0x5A, 0x0F, 0x12, 0x07, 0x09, 0x08,
            0x08, 0x24, 0x07, 0x13, 0x12, 0x6B, 0x73, 0xFF}, 16, 0},

    // Enter Command2 BK1: power-rail trims, VCOM, GIP.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, {0x8D}, 1, 0},        // VOP amplitude
    {0xB1, {0x48}, 1, 0},        // VCOM amplitude
    {0xB2, {0x89}, 1, 0},
    {0xB3, {0x80}, 1, 0},
    {0xB5, {0x49}, 1, 0},
    {0xB7, {0x85}, 1, 0},
    {0xB8, {0x32}, 1, 0},        // (our old init had 0xB9 here; espressif drops it)
    {0xC1, {0x78}, 1, 0},
    {0xC2, {0x78}, 1, 0},
    {0xD0, {0x88}, 1, 100},      // +100 ms before the big GIP block

    // GIP (Gate In Panel) sequence — drives the panel's integrated row
    // scan and timing. These are the values that failed us in rounds
    // 13–18; the stripe pattern was the gate drivers mis-pulsing without
    // these specific numbers.
    {0xE0, {0x00, 0x00, 0x02}, 3, 0},
    {0xE1, {0x05, 0xC0, 0x07, 0xC0, 0x04, 0xC0, 0x06, 0xC0,
            0x00, 0x44, 0x44}, 11, 0},
    {0xE2, {0x00, 0x00, 0x33, 0x33, 0x01, 0xC0, 0x00, 0x00,
            0x01, 0xC0, 0x00, 0x00, 0x00}, 13, 0},
    {0xE3, {0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, {0x44, 0x44}, 2, 0},
    {0xE5, {0x0D, 0xF1, 0x10, 0x98, 0x0F, 0xF3, 0x10, 0x98,
            0x09, 0xED, 0x10, 0x98, 0x0B, 0xEF, 0x10, 0x98}, 16, 0},
    {0xE6, {0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, {0x44, 0x44}, 2, 0},
    {0xE8, {0x0C, 0xF0, 0x10, 0x98, 0x0E, 0xF2, 0x10, 0x98,
            0x08, 0xEC, 0x10, 0x98, 0x0A, 0xEE, 0x10, 0x98}, 16, 0},
    {0xEB, {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x00}, 7, 0},
    {0xED, {0xFF, 0x04, 0x56, 0x7F, 0xBA, 0x2F, 0xFF, 0xFF,
            0xFF, 0xFF, 0xF2, 0xAB, 0xF7, 0x65, 0x40, 0xFF}, 16, 0},
    // 0xEF in BK1 is a 6-byte "source timing fine-trim" block the
    // datasheet leaves unnamed; present in espressif's init, absent in
    // ours up to round 18.
    {0xEF, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}, 6, 0},

    // Leave vendor mode and turn the pixel pipeline on.
    // Note: NO 0x3A COLMOD here. The panel's power-on default pixel
    // format is correct for this board's IM[3:0] strapping (16-bit RGB
    // interface on the 16 DB pins the Waveshare PCB wires). Rounds 17
    // and 18 added a 0x3A command with values 0x66 and 0x55 respectively,
    // trying to match espressif's reference for an 18-line RGB board —
    // but this Waveshare 2.1" board strictly wires 16 lines, so any
    // explicit COLMOD silently mis-programs the DB bit mapping. The
    // symptom across both values was "uniform blue + green stripe band
    // in the middle 1/3 of the screen", consistent with cross-channel
    // bit drift from the mis-programmed format.
    {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {kCmdSlpout, {0x00}, 0, 120},  // sleep out — panel needs 120 ms
    {kCmdDispon, {0x00}, 0, 0},    // display on (no post-delay per espressif)
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

    // Porch / pulse widths.
    //
    // Round 20 retuning. Rounds 14–19 used FatihErtugral's timings from
    // the sibling Waveshare 2.8" ST7701 board: pulse 8/2, back 10/18,
    // front 50/8. Every round's photo showed the same persistent symptom
    // regardless of what we changed in the init sequence — data packed
    // into the middle ~1/3 of the screen, the rest showing the panel's
    // power-on state. That's the signature of the panel's DE (data-
    // enable) window not matching what we're driving: if the back porch
    // is too tight the panel misses the first block of pixels on each
    // line, if the front porch is too wide the panel's HSYNC resync
    // arrives late and pixels at the end of the previous line bleed in.
    // An hsync_front_porch of 50 in particular is ~5× the value Espressif
    // ships for their comparable 480×480 ST7701 BSP (typical value
    // around 10–20), so it's the most suspect single number.
    //
    // Round 20 moves all six values to Espressif's typical Waveshare
    // 480×480 ST7701 setpoints: pulse 10/10, back 20/10, front 20/10.
    // Combined with the round-20 PCLK drop from 14 MHz to 10 MHz, this
    // gives the panel ample sampling margin on every pixel and sync edge.
    // The total per-frame clock count goes from 548 × 508 = 278,384 @
    // 14 MHz (50 Hz refresh) to 530 × 510 = 270,300 @ 10 MHz (37 Hz
    // refresh). 37 Hz is visibly flickery on a bench but perfectly fine
    // for bring-up and there's nothing stopping us from tightening the
    // porches once the image is coherent.
    cfg.timings.hsync_pulse_width = 10;
    cfg.timings.hsync_back_porch  = 20;
    cfg.timings.hsync_front_porch = 20;
    cfg.timings.vsync_pulse_width = 10;
    cfg.timings.vsync_back_porch  = 10;
    cfg.timings.vsync_front_porch = 10;

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
