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
DEFAULT_MSG="Round 80: close the BLE bridge end-to-end on the bench (in simulation) and ship steps 2 through 9 of the v1.5 wireless split. Sim PGNs now travel TX -> BLE GATT -> RX -> BoatState -> existing LVGL UI without code paths changing upstream of NmeaBridge. (Step 2) include/BoatBle.h defines the shared wire protocol: one 128-bit service UUID, five NOTIFY characteristics (Wind / GPS / Heading / Depth-Temp / Attitude) plus one WRITE command characteristic. All PDUs packed, each fits the default 23-byte ATT MTU (largest is 13 bytes), valid_mask bitmap per channel so missing fields are explicit instead of sentinel-encoded; fixed-point ints (deg10/kt100/m10/c10/e7) avoid IEEE-754 wire pitfalls; static_asserts pin every struct size so future packing changes fail at compile time. Bit-numbering inside Wind valid_mask is documented: 0=TWA, 1=TWS, 2=TWD, 3=AWA, 4=AWS. (Step 3) TX runs the existing simulator and acts as a NimBLE peripheral advertising as 'esp32-boat-tx'. Per-channel publish cadences match NMEA 2000 spec: GPS / Wind / Heading at 100 ms (10 Hz), Depth / Sea-T at 1 s / 2 s, Attitude at 100 ms. Two notify counters per channel — one resetting at the 5 s serial heartbeat, one monotonic for the on-device rate display. (Step 4) RX-side BLE central: new PIO env [env:waveshare_esp32s3_touch_lcd_21_ble] adds NimBLE-Arduino and -DDATA_SOURCE_BLE=1. NmeaBridge gains a third backend (under #if DATA_SOURCE_BLE) that scans for esp32-boat-tx by name, auto-connects to the first one it sees, subscribes to all five NOTIFY characteristics, and parses each PDU into the existing BoatState setters. On disconnect BoatState::invalidateLiveData() blanks every live field to NaN so the UI renders '—' instead of stale values. Status getters bleConnected() / blePeerMac() / bleRssi() / bleNotifyCount(BleChannel) feed a new 6th 'Communication' page on the RX UI (kNumPages bumped to 6 under DATA_SOURCE_BLE), exposed to Ui via ui::setBleBridge(). flash.sh rx now points at the new env. The _ble env inherits NMEA2000-library from [env]; LDF skips building it at link time because no header is included while DATA_SOURCE_BLE is set. (Step 7) TX status display with five swipe-paged screens — Primary (BLE state, notify rate, GPS, AXP2101 telemetry, uptime), Simulator (six channel state pills), PGN (per-channel measured Hz with category tints), Settings (placeholder), Communication (BLE role / advertised name / service UUID tail / client count / total notifies). Display refresh gated to 500 ms from loop(); BLE callbacks only mutate global state (LVGL isn't thread-safe and NimBLE callbacks fire from the host task). Two ble notify counters per channel so the heartbeat doesn't fight the UI over the reset window. (Step 7b) CST9217 capacitive touch wired via lewisxhe/SensorLib@^0.4.0. The chip's RESET line is behind the TCA9554 GPIO expander on EXIO6 so we pulse it ourselves before calling touch.begin() (SensorLib only sees a 'no reset pin' config). Swipe detector polls every loop tick: touch-down stamps (x0, t0); touch-up emits SWIPE_LEFT / SWIPE_RIGHT iff |dx| >= 40 px AND dt < 1000 ms; vertical strokes and slow drags ignored. 300 ms post-swipe lockout to absorb chip glitches where the CST9217 drops points mid-stroke. SensorLib upstream has a getPoint(0)-without-bounds-check bug in TouchDrvCST92xx.cpp that spams 'Invalid touch point index: 0' on every touch — patched at build time via scripts/patch_sensorlib.py wired in as extra_scripts = pre:scripts/patch_sensorlib.py in [env:nmea2k_tx]. Patch is idempotent (looks for a sentinel comment) and self-healing across libdeps wipes. (Step 8 — dead-end documented) tried bringing up the AMOLED-1.75-G's onboard LC76G GNSS over I²C (length-query / data-fetch protocol against 0x50 / 0x54 / 0x58) and UART (Serial1 on GPIO17/18 at 9600 baud). On this board variant the chip ACKs writes on 0x50 then NACKs every subsequent read; 0x54 / 0x58 never ACK; Serial1 stays silent because the R15/R16 0 Ohm jumpers between LC76G TX/RX and the ESP32 pins aren't populated. Parser plumbing kept in place: gpsI2cFails initialised to its max so the I²C poll is dormant at boot, UART drain runs every loop iteration so soldering R15/R16 will light it up with no code change. GGA parser, NMEA checksum, decimal-minute coord conversion all in the codebase ready to use. (Step 9) TX UI polish: titles 28 pt, rows 24 pt, row pitches widened across all pages, Sim page six 280x38 pill containers coloured by category pastel (Wind/GPS/Boat/Temp from the RX honeycomb palette), PGN page rows tinted by the same category palette so the visual language matches between RX and TX. Initial tap-to-toggle experiment on the Sim page was reverted — the tap detection added a 300 ms swipeLockoutMs after every touch event, which interfered with subsequent swipes; interactive Sim toggles will return when step 6 (RX -> TX command channel) lands. tx-factory-boot.log captured at repo root for reference (the binary backup taken with esptool read-flash isn't committed; .bin is gitignored). Documentation: README Status section rewritten, docs/ROADMAP marks steps 2/3/4/7/7b/9 done with step 5 hardware-blocked, step 6 deferred behind step 5, step 8 marked as a per-board dead-end. New docs/OPENPLOTTER_NMEA2000.md is a self-contained briefing for the next session that picks up step 5 — wiring the SN65HVD230 to a real NMEA 2000 backbone fed by an OpenPlotter / Signal K Raspberry Pi setup. Round 79's original scope (steps 1a-1c — PIO env scaffold, AXP2101 + I²C bus discovery, SH8601/CO5300 AMOLED + LVGL Hello banner, scripts/flash.sh role-aware helper) is folded into this commit since it was never pushed."

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
    src/ src_tx/ include/ \
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
