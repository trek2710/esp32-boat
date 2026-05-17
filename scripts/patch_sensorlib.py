# scripts/patch_sensorlib.py
#
# Idempotent pre-build patch for lewisxhe/SensorLib v0.4.x.
#
# The bug: TouchDrvCST92xx::getTouchPoints() calls
#     _touchPoints.getPoint(0).event
# unconditionally near the end of its parse loop. When the chip reports
# numPoints > 0 but every reported point has an event code the library
# filters out (which the CST9217 does for move/hold/release events — only
# event == 0x06 is accepted), pointCount ends at 0 and that getPoint(0) call
# hits TouchPoints.cpp:67 which logs
#     [E][TouchPoints.cpp:68] getPoint(): Invalid touch point index: 0
# every poll. With our 200 Hz polling loop that spams the serial output
# whenever a finger is anywhere near the screen.
#
# This patch gates the access on pointCount > 0. It's a one-liner change to
# a single file in .pio/libdeps; this script reapplies it on every build so
# `pio pkg uninstall` / scripts/package.sh's libdeps wipe doesn't bring the
# spam back.
#
# The script is idempotent: it looks for a sentinel comment we inject the
# first time we run, and skips the rewrite if it's already there. If a
# future SensorLib version reorganises the surrounding code the script will
# log a warning and exit cleanly — the build still proceeds, and if upstream
# fixed the bug we just no-op.

import os

Import("env")  # noqa: F821  — provided by PlatformIO

# Sentinel inside the patched block. If this appears we've already patched.
SENTINEL = "PATCHED (esp32-boat scripts/patch_sensorlib.py)"

# The exact unpatched fragment we look for.
OLD = (
    "        if (_touchPoints.getPoint(0).event == 0x00) {\n"
    "            _touchPoints.clear();\n"
    "            return _touchPoints;\n"
    "        }"
)

# Replacement: same logic, gated on pointCount > 0.
NEW = (
    "        // PATCHED (esp32-boat scripts/patch_sensorlib.py): the original\n"
    "        // code calls getPoint(0) unconditionally, which triggers the\n"
    '        // "Invalid touch point index: 0" log_e spam in TouchPoints.cpp\n'
    "        // when the loop above filtered every reported point (CST9217\n"
    "        // emits events other than 0x06 for move/hold/release). Guard on\n"
    "        // pointCount > 0 so the check only fires when there's actually\n"
    "        // a point to inspect.\n"
    "        if (_touchPoints.getPointCount() > 0 &&\n"
    "            _touchPoints.getPoint(0).event == 0x00) {\n"
    "            _touchPoints.clear();\n"
    "            return _touchPoints;\n"
    "        }"
)

target = os.path.join(
    env["PROJECT_LIBDEPS_DIR"], env["PIOENV"],
    "SensorLib", "src", "touch", "TouchDrvCST92xx.cpp",
)

if not os.path.exists(target):
    # SensorLib not installed yet — the next `pio run` will resolve deps
    # and trigger this script again, at which point the file will exist.
    print("[patch_sensorlib] %s not found yet — skipping (deps will be "
          "fetched and patched on the next build)" % target)
else:
    with open(target, "r", encoding="utf-8") as f:
        text = f.read()
    if SENTINEL in text:
        # Already patched. Common case after the first build.
        pass
    elif OLD in text:
        with open(target, "w", encoding="utf-8") as f:
            f.write(text.replace(OLD, NEW, 1))
        print("[patch_sensorlib] patched %s" % target)
    else:
        # SensorLib upstream may have reorganised this code. Warn and
        # let the build continue — if upstream fixed the bug we don't
        # need to patch anyway.
        print("[patch_sensorlib] WARNING: target block not found in %s. "
              "SensorLib may have changed upstream; check whether the bug "
              "is still present and update this script if so." % target)
