# Changelog

Running revision log for esp32-boat. Maintained automatically by
`scripts/update.sh` — each run prepends a dated entry here before committing,
so this file always matches the sequence of commits on the current branch.

For the authoritative history (including merges, tags, and author info) see
`git log` or <https://github.com/trek2710/esp32-boat/commits/main>.

<!-- entries below, newest first -->
- **2026-04-25** — Round 38: CST820 read uses STOP (not repeated start) between reg-pointer write and requestFrom; disable auto-sleep on begin (write 0xFF to reg 0xFE); add rate-limited touch-event log for diagnostics.
- **2026-04-25** — Round 38: CST820 read uses STOP (not repeated start) between reg-pointer write and requestFrom; disable auto-sleep on begin (write 0xFF to reg 0xFE); add rate-limited touch-event log for diagnostics.
- **2026-04-25** — Round 37: CST820 register-pointer off-by-one — start read at reg 0x01 so finger count lands at buf[1] instead of the gesture register.
- **2026-04-25** — Round 36: B&G-style Overview redesign — central compass + boat outline + 8 perimeter tiles; add CST820 touch driver wired as LVGL pointer input.
- **2026-04-25** — Round 35: vsync-gate LVGL flushes via cfg.on_frame_trans_done + binary semaphore; throttle data refresh to 10 Hz.
- **2026-04-25** — Round 34: full-frame LVGL double-buffer in PSRAM (2 × 480×480×2); remove 5-phase boot sanity bar.
- **2026-04-25** — Round 34 (re-commit): identical message to the entry above.
- **2026-04-24** — Round 33: flip ST7701 0xC2 BK0 byte-0 inversion-mode bits 0x07 → 0x37 (bits [5:4]: 00 → 11) — kills brightness banding.
- **2026-04-24** — Round 32: revert MADCTL to 0x00; do 180° rotation in flushCb via in-place buffer reverse + flipped drawBitmap coords.
- **2026-04-24** — Round 31: MADCTL 180° rotation attempt (set 0xC0) — no effect in RGB mode, superseded by round 32.
- **2026-04-24** — Round 30: replace ST7701 init sequence + RGB timings verbatim with Waveshare's authoritative config for this board.
- **2026-04-24** — Round 29: change ST7701 COLMOD from 0x55 (RGB565) to 0x66 (RGB666).
- **2026-04-24** — Round 28: revert RGB timings to FatihErtugral sibling-board values; restore 5-phase RGB/W/K boot sanity bar.
- **2026-04-24** — Round 28 (re-commit): identical message to the entry above.
- **2026-04-24** — Round 27: 16-phase walking-bit diagnostic — drive each pixel-data bit HIGH in turn to map which RGB GPIOs reach the panel.
- **2026-04-24** — Round 26: revert round 25 — ST7701 0xC2 back to baseline {0x20, 0x06}.
- **2026-04-24** — Round 25: change ST7701 0xC2 from {0x20, 0x06} to Waveshare factory {0x07, 0x0A} (chasing vertical stripes; reverted next round).
- **2026-04-24** — Round 23: extend colour-bar phases from 2 s to 5 s; log AFTER-fillColor line so the user can photograph steady state.
- **2026-04-24** — Round 22: re-add 0x3A COLMOD = 0x55 (16-bit RGB565).
- **2026-04-24** — Round 21: insert boot-time colour-bar diagnostic (RED/GREEN/BLUE/WHITE/BLACK, 2 s each).
- **2026-04-24** — Round 20: pivot from init-sequence edits to RGB-bus timing — adjust PCLK + porch widths to chase stripe pattern.
- **2026-04-23** — Round 19: replace ST7701 init table verbatim with espressif/esp-iot-solution's vendor_specific_init_default[].
- **2026-04-23** — Round 18: correct COLMOD (0x3A) from 0x66 (RGB666) to 0x55 (RGB565) to match the 16-line RGB bus.
- **2026-04-23** — Round 15: backlight pivot to raw GPIO6 PWM (was wrongly on TCA9554 IO0/LCD_RST); fix EXIO numbering off-by-one.
- **2026-04-23** — Round 15 (re-commit): identical message to the entry above.
- **2026-04-23** — Round 14: ship the real ST7701 RGB driver (esp_lcd_panel_rgb + 3-wire SPI init via TCA9554-gated CS); retire ST77916 stub.
- **2026-04-23** — Round 13: reframe — board is ST7701 RGB + TCA9554, not ST77916 QSPI + CH422G; capture the real pinout.
- **2026-04-23** — Round 12: TCA9554 IO-to-signal mapping diagnostic (walking-bit on the expander outputs).
- **2026-04-23** — Round 11: port IO-expander driver from CH422G to TCA9554; update all callsites + docs.
- **2026-04-23** — Round 10: chipset pivot — 420-pair I2C scan locks SDA=GPIO15 / SCL=GPIO7; bus has 0x15 + 0x20 + 0x51, ruling out QSPI.
- **2026-04-22** — Round 6: I2C still blind on GPIO8/9 and 10/11 — pivot to broader pin scan.
- **2026-04-22** — Round 4: SPI side clean — 64-byte sendPixels chunking, SPI3_HOST, drop spi_bus_free; init / backlight / fillColor all return OK.
- **2026-04-22** — Real display driver: hand-written ST77916 QSPI + CH422G I2C expander; replace LovyanGFX stub. (Superseded round 13.)
- **2026-04-22** — Real display driver (re-commit): identical message to the entry above.
- **2026-04-22** — Sim env builds again: include <lvgl/lvgl.h> directly; pin LovyanGFX to =1.2.0 to dodge LVGL-9 API rewrite.
- **2026-04-22** — Safe-mode firmware boots clean on real hardware (363 KB free heap); update.sh defaults to safe env until LVGL headers unbreak.
- **2026-04-21** — Add three-screen UI, PGN log, prebuilt binaries, 3D case, update.sh + CHANGELOG, and fix CI to build sim env only
- **2026-04-20** — Add three-screen UI, PGN log, prebuilt binaries, 3D case, update.sh + CHANGELOG, and fix CI to build sim env only
- **2026-04-20** — Add three-screen UI, PGN log, prebuilt binaries, 3D-printable case, and update.sh + CHANGELOG workflow
