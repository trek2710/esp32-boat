// Build-time configuration.
//
// Pin assignments, feature flags, and user-tunable constants.
// Anything you need to change per-install lives here.

#pragma once

// -----------------------------------------------------------------------------
// CAN / NMEA 2000
// -----------------------------------------------------------------------------

// ESP32-S3 TWAI (CAN) pin assignments. Verify these are free on the Waveshare
// ESP32-S3-Touch-LCD-2.1 broken-out header before soldering. If either pin is
// used by the display or touch, pick another free pair and update here.
#ifndef CAN_TX_PIN
#define CAN_TX_PIN 15
#endif

#ifndef CAN_RX_PIN
#define CAN_RX_PIN 16
#endif

// NMEA 2000 device metadata (advertised on the bus). Unique serial code
// distinguishes us from any other ESP32-based device the boat might run.
static constexpr uint32_t N2K_SERIAL_CODE      = 271828UL; // any 32-bit value
static constexpr uint16_t N2K_PRODUCT_CODE     = 2710;     // any 16-bit value
static constexpr const char* N2K_MODEL_ID      = "esp32-boat";
static constexpr const char* N2K_SW_VERSION    = "0.1.0";
static constexpr const char* N2K_MODEL_VERSION = "1";

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------

static constexpr uint16_t DISPLAY_WIDTH  = 480;
static constexpr uint16_t DISPLAY_HEIGHT = 480;

// LVGL tick in ms. Don't raise above 10.
static constexpr uint32_t LV_TICK_MS = 5;

// -----------------------------------------------------------------------------
// Feature flags
// -----------------------------------------------------------------------------

// When set, NmeaBridge feeds the UI fake data instead of talking to the bus.
// Useful for desk development before the transceiver is wired up.
#ifndef SIMULATED_DATA
#define SIMULATED_DATA 0
#endif

// Step 4: when set, NmeaBridge runs as a BLE central instead of attaching
// to NMEA 2000. It scans for "esp32-boat-tx", subscribes to the five
// telemetry NOTIFY characteristics defined in include/BoatBle.h, and
// pushes parsed values into BoatState. Mutually exclusive with
// SIMULATED_DATA (the build will fail with a #error if both are set).
// Selected via `-e waveshare_esp32s3_touch_lcd_21_ble`.
#ifndef DATA_SOURCE_BLE
#define DATA_SOURCE_BLE 0
#endif

#if SIMULATED_DATA && DATA_SOURCE_BLE
#error "SIMULATED_DATA and DATA_SOURCE_BLE are mutually exclusive"
#endif

// Set to 1 and populate `secrets.h` (WiFi credentials) to enable OTA.
#ifndef ENABLE_OTA
#define ENABLE_OTA 0
#endif

// When set, `ui::begin()` and `ui::tick()` are skipped entirely. The firmware
// still runs Serial logging and the NMEA / sim bridge, just without any LVGL
// or display init. Use this to isolate "is the boot loop caused by the
// display driver?" from "is something else broken?" when bringing up on new
// hardware. Build with `-e waveshare_esp32s3_touch_lcd_21_safe` or add
// `-DDISPLAY_SAFE_MODE=1` to any env's build_flags.
#ifndef DISPLAY_SAFE_MODE
#define DISPLAY_SAFE_MODE 0
#endif
