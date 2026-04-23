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
DEFAULT_MSG="Display bring-up round 11: port expander driver from CH422G to TCA9554 and update every callsite / doc to match the pivot. Round 10's log produced two payoffs: (a) SDA=GPIO15 is locked, and (b) SCL=GPIO7 is locked indirectly — every SCL candidate produced the same 5-device ACK set (0x15, 0x20, 0x51, 0x6B, 0x7E), which is a known Arduino-ESP32 quirk where Wire.end() releases the driver but not the GPIO matrix routing, so the *first* successful begin (SCL=7) physically drove the bus for every subsequent iteration. The real bus fingerprint is five devices: CST820 touch, TCA9554 expander, PCF85063 RTC, probably a QMI8658 IMU at 0x6B, and an unknown/spurious thing at 0x7E. This round lands the driver work that round 10 only *documented*: (1) new src/display/tca9554.{h,cpp} implementing class Tca9554 with the standard register-pointer protocol (write reg 0x03 = 0x00 to make all 8 IOs outputs, then write reg 0x01 to drive them); public API (begin/writeOutput/setBits/clearBits/shadow) is deliberately identical to the old Ch422g class so the panel code swaps without ceremony. (2) Deleted src/display/ch422g.{h,cpp}. (3) st77916_panel.{h,cpp} now takes a Tca9554& instead of a Ch422g&, references TCA9554_BIT_LCD_BL / TCA9554_BIT_LCD_RST. (4) Ui.cpp now declares 'display::Tca9554 g_expander', includes display/tca9554.h, logs 'TCA9554.begin()' instead of 'CH422G.begin()', and its comment header is rewritten to state plainly that the board is the ST7701+TCA9554 variant (kept the ST77916 driver for now; round 12 rips it out). (5) Replaced round 10's 420-pair SCL-verification block with a single 127-address boot-time bus enumeration on the locked SDA=15/SCL=7 pair — still prints every ACKed address so we can see at a glance whether the bus came up with the expected five devices. (6) display_pins.h: CH422G_* symbols deleted, TCA9554_* symbols (addr 0x20, reg 0x03 CONFIG, reg 0x01 OUTPUT, bits IO0=LCD_BL / IO1=TP_RST / IO2=LCD_RST / IO3=SD_CS) made primary, pivot-plan updated, 0x6B/0x7E documented as best-guess IMU + unknown. (7) docs/WIRING.md rewritten to reflect the pivot (TCA9554 table, updated SDA/SCL to 15/7). Expected round-11 boot log: I2C bus enumeration shows ACKs at 0x15/0x20/0x51/0x6B/0x7E, then 'tca9554 ok (addr=0x20, config=0x00, output=0x00)' where round 10 got 'ch422g mode-register write to 0x24 NACKed'. The ST77916 block will still claim success while driving nothing — that's round 12's problem."

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
