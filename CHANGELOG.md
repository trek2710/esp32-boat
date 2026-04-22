# Changelog

Running revision log for esp32-boat. Maintained automatically by
`scripts/update.sh` — each run prepends a dated entry here before committing,
so this file always matches the sequence of commits on the current branch.

For the authoritative history (including merges, tags, and author info) see
`git log` or <https://github.com/trek2710/esp32-boat/commits/main>.

<!-- entries below, newest first -->
- **2026-04-22** — Sim env builds again: fix LVGL header hijack (include <lvgl/lvgl.h> directly, add .pio/libdeps/<env>/ to -I) and pin LovyanGFX to =1.2.0 to dodge the 1.2.20 LVGLfont rewrite against LVGL 9 API. Restore sim as default build env for update.sh and CI.
- **2026-04-22** — Safe-mode firmware now boots cleanly on real hardware (heartbeats + 363KB free heap stable); update.sh defaults to building safe env until LVGL headers unbreak
- **2026-04-21** — Add three-screen UI, PGN log, prebuilt binaries, 3D case, update.sh + CHANGELOG, and fix CI to build sim env only
- **2026-04-20** — Add three-screen UI, PGN log, prebuilt binaries, 3D case, update.sh + CHANGELOG, and fix CI to build sim env only
- **2026-04-20** — Add three-screen UI, PGN log, prebuilt binaries, 3D-printable case, and update.sh + CHANGELOG workflow
