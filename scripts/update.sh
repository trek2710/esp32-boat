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
DEFAULT_MSG="Display bring-up round 15: fix the two wrong assumptions that kept the round-14 driver producing a black screen. Round 14 compiled clean, flashed fine, and every log line reported success — TCA9554 acked, 37-command ST7701 init sequence sent, esp_lcd_new_rgb_panel up at 14 MHz PCLK with a 480x480 PSRAM framebuffer, backlight bit set, LVGL alive — but the panel still showed nothing. A second read of the LovyanGFX discussion #630 config for this exact board (the same source we used for the RGB pin map) surfaced two off-by-one errors we'd been carrying since round 13. First: the backlight is not on the TCA9554 at all. It lives on RAW GPIO6 with PWM via Waveshare's ledcAttach(6, 20000, 10). Our own boot-log GPIO idle-state scan showed gpio 6 sitting LOW with nothing driving it, which is exactly what an unpowered backlight FET gate looks like — the clue was in our own logs, we just didn't know what to do with it. Second: the TCA9554 EXIO labels in the LGFX config are 1-indexed helper names, not direct bit numbers: EXIO1 is bit 0, EXIO2 is bit 1, EXIO3 is bit 2, EXIO8 is bit 7. Round 13 read them as direct bit numbers which shifted every signal up by one, so round 14's panel-reset pulse was actually hitting the touch-reset line and the ST7701 was never reset. It stayed in its power-on idle state and ignored the init sequence entirely, which explains both the dark screen and why the backlight bit we were toggling (TCA9554 IO0 = actually LCD_RST) made no visible difference. Round 15 fixes are all in display_pins.h and st7701_panel.cpp: the TCA9554 bit-mask constants shift down by one (bit 0 = LCD_RST, bit 1 = TP_RST, bit 2 = LCD_CS, bit 7 = BUZZER untouched because bit 7 is bit 7 either way which is the accidental reason round-12 beep detection produced the right buzzer mapping), TCA9554_BIT_LCD_BL is deleted entirely, new BACKLIGHT_PIN / BACKLIGHT_PWM_CH / BACKLIGHT_PWM_FREQ / BACKLIGHT_PWM_RES / BACKLIGHT_PWM_FULL constants describe the GPIO6 PWM setup, and St7701Panel::begin() replaces the TCA9554 backlight-enable call with ledcSetup(0, 20000, 10) + ledcAttachPin(6, 0) + ledcWrite(0, 1023). Arduino-ESP32 2.0.16 doesn't have the ledcAttach convenience API (that's 3.x) so we use the 3-function form. WIRING.md display and TCA9554 tables are regenerated with the corrected mapping, tca9554.cpp safe-state comment matches, and the display_pins.h header gains a round-15 pivot note that explains the mistake in full so future readers don't undo it. Next flash should paint the LVGL boot screen; if it's still dark, the remaining suspects in decreasing likelihood are (a) RGB timing porch values off, resolvable by widening h_back_porch / v_back_porch, (b) wrong ST7701 init sequence for this specific panel sub-variant, (c) RGB data-pin order wrong on one of R/G/B (would show as correct brightness but garbled colour, not black, so unlikely)."

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
