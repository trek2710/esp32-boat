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
DEFAULT_MSG="Display bring-up round 14: ship the real ST7701 RGB driver and retire the ST77916 QSPI stub. Round 13 established the chipset pivot on paper; round 14 turns that into running code. New files src/display/st7701_panel.h and st7701_panel.cpp implement the panel against ESP-IDF's esp_lcd_panel_rgb, which ships inside the Arduino-ESP32 2.0.16 IDF 4.4.7 fork so no toolchain bump is needed. Bring-up runs in six steps inside begin(): assert LCD_RST through TCA9554 IO1, release it and wait 120 ms for the ST7701 PLL, bit-bang the 37-command vendor init sequence over a 3-wire SPI link built from SCK=GPIO2 + SDA=GPIO1 + CS=TCA9554 IO3 using 9-bit transactions (one DCX bit then 8 data bits MSB-first), construct esp_lcd_new_rgb_panel with the full 16-bit RGB pin map and porch timings from display_pins.h at 14 MHz PCLK with a PSRAM-backed 480x480x2 framebuffer, call esp_lcd_panel_init to push the first frame and start PCLK, then raise LCD_BL on TCA9554 IO0 last so the first visible state is a blacked-out framebuffer rather than DRAM garbage. The init table is sourced from Nicolai-Electronics/esp32-component-st7701 vendor_specific_init_default[] — same BK0/BK1 gamma + GIP sequence that the Waveshare reference demo uses, just spelled out as a C array instead of hidden inside a vendor LCD_Driver component. drawBitmap() and fillColor() are thin wrappers over esp_lcd_panel_draw_bitmap so Ui.cpp's flush_cb keeps the identical signature it had for ST77916 — the only line that changed in Ui.cpp is the #include and class type swap from display::St77916Panel to display::St7701Panel plus a log-message update. st77916_panel.h and st77916_panel.cpp are deleted. If PSRAM framebuffer allocation fails (8 MB board but a runaway task could still exhaust it) begin() returns false and the LVGL flush becomes a no-op rather than crashing. First compile attempt also flagged a struct-field mismatch: cfg.bounce_buffer_size_px does not exist in IDF 4.4.7 — that field was added in IDF 5.x as part of the SRAM bounce-buffer fast path for high-PCLK PSRAM framebuffers. At 14 MHz we don't need it anyway, so the line is removed and replaced with a comment pointing at the IDF version pivot for future reference. Next flash should paint the LVGL boot screen; if it's still dark, suspects are: (a) IO0=LCD_BL hypothesis, verifiable by driving that bit high in isolation, (b) SPI init pins GPIO1/GPIO2 wrong on this board revision, verifiable by checking factory firmware pin usage, (c) timing porch values mismatched, resolvable by widening h_back_porch / v_back_porch."

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
