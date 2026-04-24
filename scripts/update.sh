#!/usr/bin/env bash
# update.sh — one-shot "ship it" script for esp32-boat.
#
# What it does, in order:
#   1. Rebuilds the sim firmware and refreshes binaries/*.bin so the
#      committed pre-built images always match the source.
#   2. Prepends a dated entry to CHANGELOG.md with the commit message.
#   3. Shows you what changed (git status + short diff stat).
#   4. Stages every change under tracked folders (src/, scripts/, binaries/,
#      hardware/, docs/, README.md, .gitignore, platformio.ini, include/,
#      CHANGELOG.md).
#   5. Commits with the message you pass in (or the DEFAULT_MSG below,
#      which Claude updates each time the repo changes).
#   6. Pushes to origin on the current branch.
#
# Usage:
#   ./scripts/update.sh                          # use DEFAULT_MSG below
#   ./scripts/update.sh "custom commit message"  # override
#   ./scripts/update.sh -n                       # skip firmware rebuild
#   ./scripts/update.sh -n "docs-only tweak"     # skip build + custom msg
#
# Exit codes:
#   0 success    1 build failed    2 git failed    3 user aborted

# ============================================================================
# DEFAULT_MSG — the commit message for the current set of uncommitted
# changes. Claude updates this each time it modifies the repo, so you can
# just run `./scripts/update.sh` with no arguments and get a meaningful
# commit. Override by passing a message as the first positional argument.
# ============================================================================
DEFAULT_MSG="Display bring-up round 20: pivot from init-sequence edits to RGB-bus timing. Round 19 shipped a verbatim port of espressif's ST7701 init sequence and the display output did not change from round 18 — same uniform-blue background, same column of green+blue stripes compressed into the middle ~1/3 of the screen. The fact that a completely different init sequence produced an identical visual symptom is diagnostic: the problem is not in what commands we send to the panel, it is in HOW we drive the 16 RGB data lines and their sync/porch signals afterward. Round 20 retunes the RGB peripheral in two parts. (a) PCLK drops from 14 MHz to 10 MHz in display_pins.h::RGB_PCLK_HZ. Rounds 14–19 used 14 MHz because that's what FatihErtugral's working config for the sibling Waveshare 2.8\" ST7701 board ships; but on our 2.1\" 480×480 panel 14 MHz is apparently outrunning the source-driver sampling window given our generous front porch. 10 MHz is well inside the ST7701S-datasheet-guaranteed band (~3 MHz to ~20 MHz) and biases for 'works' over 'max refresh'. We can step back up to 12–14 MHz once the image is coherent. (b) Porches move from FatihErtugral's values to Espressif's Waveshare-BSP typical values. Specifically: hsync_pulse_width 8→10, hsync_back_porch 10→20, hsync_front_porch 50→20, vsync_pulse_width 2→10, vsync_back_porch 18→10, vsync_front_porch 8→10 in st7701_panel.cpp::initRgbPanel(). The single largest delta is hsync_front_porch dropping from 50 to 20 — 50 was ~5x the value Espressif uses for comparable 480×480 ST7701 BSPs and is the strongest suspect for why the panel's HSYNC resync arrives too late per line, with pixels at the end of one line bleeding into the start of the next. The new porch+PCLK combo gives total 530×510 clocks per frame @ 10 MHz = 37 Hz refresh (down from 50 Hz); flickery on a bench but harmless for bring-up. Init sequence is untouched from round 19 (verbatim espressif port). Pin order, backlight, and TCA9554 reset sequence all unchanged. If round 20 visibly changes the stripe pattern — width, shape, or position — timing was involved and round 21 can fine-tune from there. If round 20 shows no change at all (same as rounds 18/19), timing is not the issue and the suspect shifts to pin order or LV_COLOR_16_SWAP polarity; round 21 would then reverse data_gpio_nums to test whether the ESP-IDF 4.4.7 RGB peripheral puts bit 0 on the last array slot rather than the first. Historical context preserved from round 19: that round replaced our round-13 init table (stitched from Nicolai-Electronics + generic ST7701S datasheet) with a verbatim port of espressif/esp-iot-solution/esp_lcd_st7701_rgb.c::vendor_specific_init_default[]. Key fixes in that port were (1) BK3 entry via 0xFF {...,0x13} instead of a stray MIPI NORON, (2) entire 0xE0..0xED GIP block replaced with espressif's known-good values, (3) dropped the 0x3A COLMOD addition from rounds 17/18. None of those changes moved the visual symptom, which is what triggered the round-20 pivot. Round 18's photo showed a uniform blue background with a vertical column of green+blue stripes spanning the middle ~1/3 of the screen — a textbook gate-in-panel (GIP) timing mismatch where most columns of the panel's source drivers never get the right gate pulse and stay in their power-on blue state, while a narrow band actually scans the data we send. Rounds 13–18 used an init table stitched together from Nicolai-Electronics/esp32-component-st7701 + the generic ST7701S datasheet sequence; cross-checking that table against espressif's known-good sequence (pulled from the current master of esp-iot-solution's esp_lcd_st7701 driver) surfaced three systemic problems beyond individual byte differences: (1) Our init opened with '0xFF {0x77,0x01,0x00,0x00,0x00}' then sent '0x13' as a standalone command. We intended 0x13 as a Command2 BK3 selector but the wire bits were the MIPI standard NORON (Normal Display On). Espressif opens with '0xFF {0x77,0x01,0x00,0x00,0x13}' — BK3 entered via the 0xFF parameter — then sends '0xEF {0x08}' in BK3. Our 0xEF landed in normal mode where the command has no effect. (2) The entire 0xE0..0xED GIP block differed. GIP values are what tell the gate drivers when to pulse each row; wrong values → rows that never activate → the mid-band stripe. Espressif's values are the known-good ones for the 480x480 Waveshare ST7701 family. Our 0xB0/0xB1 (BK0 positive/negative gamma) and the BK1 power-rail constants (0xB0 VOP 0x5D→0x8D, 0xB1 VCOM 0x2D→0x48, 0xB2 0x07→0x89, 0xB5 0x08→0x49, 0xB8 0x20→0x32) were also wrong, and an 0xEF 6-byte source-timing fine-trim block in BK1 was missing entirely. (3) Rounds 17 and 18 added a 0x3A COLMOD command (0x66 then 0x55) on the theory that pixel-format was wrong; espressif OMITS 0x3A entirely because the panel's power-on default pixel format is correct for its hardware strapping (IM[3:0] pins on the panel module select 16-bit RGB interface mode). Explicitly setting a non-default COLMOD silently misaligns bits on the RGB bus, which is consistent with the stripe pattern persisting across both 0x66 and 0x55 attempts. Round 19 drops the 0x3A addition. Also removed: the stray '{0x13, {}, 0, 0}' NORON send, the '0xB9 {0x10}' line that isn't in the reference. Also in st7701_panel.cpp: kCmdNoron constant deleted (no longer used), kCmdDispon post-delay changed from 50 ms to 0 ms per espressif. Only the init table changed this round — RGB timings, pin order, backlight, and TCA9554 reset sequence all stay exactly as round 18. If round 19 still stripes, the suspect shifts from 'software init' to 'RGB bus / timing' and round 20 should pivot to: (a) reversing data_gpio_nums pin order (some ESP-IDF 4.4.x builds put bit 0 on the LAST data_gpio_nums slot, not the first), (b) widening hsync_back_porch from 10 to 20 and reducing hsync_front_porch from 50 to 20 to match Espressif's typical 480x480 ST7701 timings, (c) dropping PCLK from 14 MHz to 10 MHz to rule out bus-bandwidth issues. Round 17 added the COLMOD command for the first time (rounds 13–16 had omitted it) and the stripe pattern's dominant colour shifted from 'mostly blue' in round 16 to 'more green and blue' in round 17 — evidence the panel IS responding to COLMOD, but 0x66 was the wrong value for our wiring. 0x66 is what espressif/esp-iot-solution's ST7701 driver uses, but espressif's reference board wires 18 data lines; Waveshare's ESP32-S3-Touch-LCD-2.1 only has 16 (B0..B4, G0..G5, R0..R4). When COLMOD=0x66 the ST7701 expects DB[17..12]=R, DB[11..6]=G, DB[5..0]=B; driving only DB[15..0] means the bit positions shift — our pixel's R MSB (bit 15) lands in the panel's R3 slot, R LSB (bit 11) lands in G5, G MSB (bit 10) lands in G4, etc. That cross-channel bit-shift is exactly the pattern that produces repeating-but-mis-coloured vertical stripes instead of coherent pixels. COLMOD=0x55 sets both the RGB (DPI) and MCU (DBI) interfaces to 16-bit 5-6-5 mode, which matches our 16 data lines exactly: DB[15..11]=R[4..0], DB[10..5]=G[5..0], DB[4..0]=B[4..0] — the natural RGB565 layout the ESP32 RGB peripheral puts on the wire with LV_COLOR_DEPTH=16 and LV_COLOR_16_SWAP=1. The ST7701S datasheet section 10.2.19 lists 0x55 as valid; LovyanGFX's config for Waveshare 16-line ST7701 boards uses it too. Single-byte change: in src/display/st7701_panel.cpp::kInitCmds[], {0x3A, {0x66}, 1, 0} becomes {0x3A, {0x55}, 1, 0}. The long-form comment above the entry now explains the 18bpp-vs-16bpp mismatch in full so the next developer doesn't flip it back to 0x66 chasing the espressif reference. Round 17 context preserved: that round also corrected 0xC0 display-line setting from 0x63 (800 lines) to 0x3B (480 lines) using the (NL+1)*8 formula — that fix stays in place since the 800-line mis-programming was independently wrong. If round 18 still shows stripes, remaining suspects in decreasing likelihood are: (a) horizontal timing — hsync_front_porch of 50 is unusually large vs Espressif's typical 20, widening the back porch at the expense of front porch could align the panel's sampling window (try hsync_pulse_width 10 / hsync_back_porch 20 / hsync_front_porch 20); (b) data_gpio_nums pin order — the ESP32 RGB peripheral might put bit 0 on DB[15] instead of DB[0] on some IDF versions, test by reversing the array; (c) COLMOD=0x50 as an alternative 16bpp value that some ST7701 revisions prefer over 0x55. Round 16 flipped PCLK polarity on the theory that round 15's stripes came from sampling data on the wrong clock edge; that hypothesis did not pan out (post-round-16 flash showed the same stripes, mostly blue hue). Re-reading our init table against espressif/esp-iot-solution's esp_lcd_st7701 driver and a handful of community ST7701 + Waveshare references surfaced two actual problems we had carried since round 13. (1) The init sequence never sends 0x3A. On power-up the ST7701 sits in whatever default pixel-format state its ROM leaves it in, which for RGB-interface operation is ambiguous across ST7701 revisions — some boot into a layout that pulls bits from our 16 data wires in a way that doesn't match RGB565. The mis-alignment between wire-level bits and the panel's internal pipeline bits is a textbook cause of the 'same data repeats as a vertical stripe with a dominant blue channel' pattern the monitor showed: when an RGB565 word (R5 G6 B5 = 16 bits) is read into an 18-bit RGB666 path without the panel being told it's in 18bpp mode, the B channel's LSBs land where G's MSBs would go, R's MSBs get truncated, and what survives is mostly the (large) low-bit blue values. Round 17 inserts {0x3A, {0x66}, 1, 0} — 18-bit RGB666 in RGB-interface mode — right after the 0xFF that exits Command2 vendor mode and before SLPOUT+DISPON. 0x66 is the value the espressif esp-iot-solution ST7701 driver uses, as does Waveshare's own vendor config; the alternative 0x55 (16bpp RGB565) is documented in the ST7701S datasheet but several revisions ship with it unsupported in RGB-interface mode, so 0x66 is the safer match. (2) 0xC0 Display Line Setting: byte 1 encodes (NL+1)*8 = number of display lines. 0x63 = 99, which programs the panel for (99+1)*8 = 800 lines. 480 lines requires NL = 59 = 0x3B, which is what round 17 sets. Driving a 480-line panel while it's internally gated for 800 lines smears rows across the physical array, which is also consistent with the 'middle 50% of the screen' framing in the stripe symptom. Both changes are in src/display/st7701_panel.cpp::kInitCmds[]: new 0x3A entry inserted, existing 0xC0 parameter changed from 0x63 to 0x3B. Long-form comments above both entries record the round-17 narrative so a future reader (likely us in round 20-something) doesn't undo the fix. Touch-controller side effect preserved from round 16: CST820 at 0x15 still ACKed-then-stopped after round 15's safe-state TCA9554 writes held TP_RST (bit 1) asserted low; not a v1 blocker because there's no touch code yet, but round 18+ will need to raise that bit before talking to the CST820. Historical context preserved from round 15: dark-screen fix moved backlight from TCA9554 to raw GPIO6 PWM (ledcSetup+ledcAttachPin+ledcWrite, 20 kHz, 10-bit, full duty) and shifted all TCA9554 bit-mask constants down by one (bit 0 = LCD_RST, bit 1 = TP_RST, bit 2 = LCD_CS, bit 7 = BUZZER) after realising the LGFX #630 EXIO1..EXIO8 labels are 1-indexed helper names mapping to P0..P7. Round 14 history preserved: removed cfg.bounce_buffer_size_px = 0 (that field was added in IDF 5.x; Arduino-ESP32 2.0.16 bundles IDF 4.4.7). Round 16 history preserved: pclk_active_neg 1→0 and vsync_pulse_width 8→2, vsync_front_porch 20→8 to align with FatihErtugral's ESP-IDF driver for the sibling Waveshare 2.8 inch ST7701 board. If round 17 still shows stripes, remaining suspects in decreasing likelihood are (a) RGB data-pin order reversed on one of R/G/B channels (LGFX #630 lists R0..R4, G0..G5, B0..B4 in ascending-bit order but some community repos use descending; swapping both channels' order independently is a cheap test), (b) horizontal timing porch values too tight for this panel sub-variant (try widening hsync_back_porch from 10 to 20 and hsync_front_porch from 50 to 30 to match espressif's defaults), (c) ST7701 init sequence has a gamma or GIP value that doesn't match this cell's glass (B0/B1/E5/E8 blocks)."

set -euo pipefail

# ---- parse args -------------------------------------------------------------
SKIP_BUILD=0
if [[ "${1:-}" == "-n" || "${1:-}" == "--no-build" ]]; then
    SKIP_BUILD=1
    shift
fi
MSG="${1:-${DEFAULT_MSG}}"

# ---- move to repo root ------------------------------------------------------
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO_ROOT}"

# ---- sanity check: are we actually in a git repo? ---------------------------
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "ERROR: ${REPO_ROOT} is not a git working tree." >&2
    exit 2
fi
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
echo "==> Repo:    ${REPO_ROOT}"
echo "==> Branch:  ${BRANCH}"
echo "==> Message: ${MSG}"

# ---- 1. rebuild firmware (unless --no-build) --------------------------------
# Default env for the packaged binaries is SIM: compiles LVGL + LovyanGFX in
# full, feeds the UI fake NMEA values, and is the one we iterate on day-to-day.
# Override to "safe" for the LVGL-less diagnostic image, or "waveshare_..." for
# production (the prod env still needs an S3-capable NMEA2000 CAN backend
# before it will link — see platformio.ini).
BUILD_ENV="${BUILD_ENV:-sim}"
if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    echo
    echo "==> Rebuilding ${BUILD_ENV} firmware (use -n to skip, or BUILD_ENV=sim ./scripts/update.sh to override)"
    ./scripts/package.sh "${BUILD_ENV}"
else
    echo
    echo "==> Skipping firmware rebuild (--no-build)"
fi

# ---- 2. prepend a dated entry to CHANGELOG.md -------------------------------
# CHANGELOG.md is a human-readable revision list, newest first. Each run of
# this script prepends exactly one entry so the file stays in lock-step with
# actual commits (as opposed to being hand-maintained, which drifts).
CHANGELOG="CHANGELOG.md"
SENTINEL="<!-- entries below, newest first -->"
DATE_STR="$(date +%Y-%m-%d)"
ENTRY="- **${DATE_STR}** — ${MSG}"

if [[ ! -f "${CHANGELOG}" ]]; then
    cat > "${CHANGELOG}" <<EOF
# Changelog

Running revision log for esp32-boat. Maintained automatically by
\`scripts/update.sh\` — each run prepends a dated entry here before committing,
so this file always matches the sequence of commits on the current branch.

${SENTINEL}
${ENTRY}
EOF
    echo
    echo "==> Created ${CHANGELOG} with first entry."
else
    TMP="$(mktemp)"
    awk -v sentinel="${SENTINEL}" -v entry="${ENTRY}" '
        BEGIN { done = 0 }
        { print }
        $0 == sentinel && !done { print entry; done = 1 }
    ' "${CHANGELOG}" > "${TMP}"
    mv "${TMP}" "${CHANGELOG}"
    echo
    echo "==> Prepended entry to ${CHANGELOG}:"
    echo "    ${ENTRY}"
fi

# ---- 3. show the damage -----------------------------------------------------
echo
echo "==> Working-tree changes:"
git status --short
echo
echo "==> Diff summary:"
git diff --stat HEAD || true

# bail cleanly if there's nothing to commit
if [[ -z "$(git status --porcelain)" ]]; then
    echo
    echo "==> Nothing to commit. Working tree clean. Done."
    exit 0
fi

# ---- 4. stage everything under tracked project paths ------------------------
# We explicitly list paths rather than `git add -A` so a stray file in the
# repo root (e.g. a scratch .env, a build log) doesn't sneak into the commit.
echo
echo "==> Staging changes"
git add -- \
    src/ include/ \
    scripts/ \
    binaries/ \
    hardware/ \
    docs/ \
    .github/ \
    README.md LICENSE .gitignore platformio.ini \
    CHANGELOG.md \
    2>/dev/null || true

# Tell the user what actually got staged.
echo
echo "==> Staged for commit:"
git diff --cached --stat

# If nothing is staged (everything in the diff was in a path we don't auto-add),
# stop and let the user run `git add` manually.
if [[ -z "$(git diff --cached --name-only)" ]]; then
    echo
    echo "WARNING: no files were auto-staged. Changes exist but are outside"
    echo "         the paths update.sh manages. Run 'git add <file>' yourself"
    echo "         and re-run this script, or commit manually."
    exit 2
fi

# ---- 5. commit --------------------------------------------------------------
echo
echo "==> Committing: ${MSG}"
git commit -m "${MSG}"

# ---- 6. push ----------------------------------------------------------------
echo
echo "==> Pushing to origin/${BRANCH}"
# -u sets upstream on first push; harmless on subsequent pushes.
if ! git push -u origin "${BRANCH}"; then
    echo
    echo "ERROR: push failed. Commit is local on ${BRANCH} — fix the remote" >&2
    echo "       issue and run 'git push origin ${BRANCH}' when ready." >&2
    exit 2
fi

echo
echo "==> Done. https://github.com/trek2710/esp32-boat/commits/${BRANCH}"
echo "    See CHANGELOG.md for the running revision list."
