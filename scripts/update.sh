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
DEFAULT_MSG="Round 76: Main display visual overhaul plus wind-indicator rebinding. (1) kCompassSize 460→480 so the dial fills the round panel; r_mod on all four no-go arcs goes -22→0 so the wedges are flush to the bezel (user: 'the wedge is not at the border of the screen anymore'). (2) Cone (struct field still twd_arrow for diff-min) reshaped: kConeW 50→25, kConeH 140→182 (+30%), kConeVisibleBase 90→117. Inner colour blue→yellow. Re-bound to s.awa in refreshMain (previously s.twd) — user named this 'the apparent wind direction arrow'. (3) Outer-rim triangle (target_marker) gets kTgtH 230→241 so the tip sits at screen radius 240 — 'fully at the edge'. Colour blue→yellow. Re-bound from the round-55 hardcoded kTargetCourse=60° stub to s.twd in refreshMain — user named this 'the true wind triangle indicator'. (4) BSPD readout moved from the stern offset (0,145) to the centre (0,0) with the same DEPTH-style subtitle/value/unit triple ('BSPD' / value / 'kn'). Sits inside the new flat-aft hull; black-on-white reads cleanly through the unfilled hull interior. (5) Boat hull redrawn: recentred from x=230→x=240 to match the wider widget; widest body half-width 50→40 px (20% slimmer); flat horizontal aft from (210,370)→(270,370), 60 px wide vs. the original ~10 px (= 50 px wider, as specified). (6) Heading box widened 64→100 px; new headingCardinal() helper maps degrees-true to an 8-point cardinal abbreviation (N/NE/E/SE/S/SW/W/NW); refreshMain formats the readout as '038° NE'."

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
