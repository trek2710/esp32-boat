#!/usr/bin/env bash
# package.sh — build the sim firmware and drop the binaries into binaries/
# so they can be committed to the repo for easy flashing.
#
# Usage:
#   ./scripts/package.sh            # builds the sim env (default)
#   ./scripts/package.sh sim        # same as above, explicit
#   ./scripts/package.sh prod       # build the production env (needs S3 CAN backend)
#
# After this runs you'll find, in binaries/:
#   esp32-boat-<env>-bootloader.bin  (flash at 0x0000)
#   esp32-boat-<env>-partitions.bin  (flash at 0x8000)
#   esp32-boat-<env>-boot_app0.bin   (flash at 0xe000)
#   esp32-boat-<env>-firmware.bin    (flash at 0x10000)
#   esp32-boat-<env>-merged.bin      (one-file image — flash at 0x0)
#
# The merged image is the easiest way for someone without a toolchain to
# flash the board — see binaries/README.md.

set -euo pipefail

CHOICE="${1:-sim}"
case "${CHOICE}" in
    sim|SIM)
        ENV="waveshare_esp32s3_touch_lcd_21_sim"
        TAG="sim"
        ;;
    safe|SAFE)
        ENV="waveshare_esp32s3_touch_lcd_21_safe"
        TAG="safe"
        ;;
    prod|PROD|production)
        ENV="waveshare_esp32s3_touch_lcd_21"
        TAG="prod"
        ;;
    *)
        echo "usage: $0 [sim|safe|prod]" >&2
        exit 1
        ;;
esac

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/.pio/build/${ENV}"
LIBDEPS_DIR="${REPO_ROOT}/.pio/libdeps/${ENV}"
OUT_DIR="${REPO_ROOT}/binaries"
mkdir -p "${OUT_DIR}"

# ---- cache hygiene ---------------------------------------------------------
# PlatformIO reuses whatever is in .pio/libdeps/<env>/ even after lib_deps
# in platformio.ini is tightened. We got bitten HARD by this on LVGL: 8.4.0
# was cached under libdeps and kept being picked up after we pinned to 8.3.x,
# producing identical compile errors across multiple rebuilds.
#
# Auto-clean when platformio.ini is newer than libdeps/, i.e. the user has
# edited a pin since the last resolve. Override with CLEAN_LIBDEPS=0 if you
# explicitly want to reuse the cached libs (e.g. offline, in a hurry).
INI="${REPO_ROOT}/platformio.ini"
AUTO_CLEAN=0
if [[ -d "${LIBDEPS_DIR}" && -f "${INI}" && "${INI}" -nt "${LIBDEPS_DIR}" ]]; then
    AUTO_CLEAN=1
fi
if [[ "${CLEAN_LIBDEPS:-${AUTO_CLEAN}}" == "1" && -d "${LIBDEPS_DIR}" ]]; then
    echo "==> Clearing ${LIBDEPS_DIR} (platformio.ini changed — forcing re-resolve)"
    rm -rf "${LIBDEPS_DIR}"
fi

echo "==> Building PlatformIO env: ${ENV}"
cd "${REPO_ROOT}"
pio run -e "${ENV}"

# PlatformIO drops bootloader/partitions/firmware into the build dir.
# boot_app0.bin lives in the Arduino framework package.
BOOT_APP0="$(find "$(pio pkg show --global --storage-dir 2>/dev/null || echo "${HOME}/.platformio")" \
    -path '*framework-arduinoespressif32/tools/partitions/boot_app0.bin' 2>/dev/null | head -n1)"
if [[ -z "${BOOT_APP0}" ]]; then
    # Fall back to the conventional path.
    BOOT_APP0="${HOME}/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
fi

for f in bootloader.bin partitions.bin firmware.bin; do
    if [[ ! -f "${BUILD_DIR}/${f}" ]]; then
        echo "ERROR: expected ${BUILD_DIR}/${f} — did the build succeed?" >&2
        exit 1
    fi
    cp -v "${BUILD_DIR}/${f}" "${OUT_DIR}/esp32-boat-${TAG}-${f}"
done

if [[ -f "${BOOT_APP0}" ]]; then
    cp -v "${BOOT_APP0}" "${OUT_DIR}/esp32-boat-${TAG}-boot_app0.bin"
else
    echo "WARNING: boot_app0.bin not found; merged.bin will be skipped"
    exit 0
fi

echo "==> Creating merged image"
# esptool + pyserial can live in one of three places, in order of preference:
#   1. PlatformIO's bundled virtualenv (penv) — always has both preinstalled.
#   2. `pio` on PATH — we ask it for its python interpreter.
#   3. Any other python3 on PATH — only works if the user pip-installed
#      esptool + pyserial themselves.
ESPTOOL_PY="${HOME}/.platformio/packages/tool-esptoolpy/esptool.py"

# --- 1. Probe common penv locations -----------------------------------------
PIO_PENV_PY=""
for candidate in \
    "${HOME}/.platformio/penv/bin/python" \
    "${HOME}/.platformio/penv/bin/python3" \
    "${HOME}/.platformio/python3/bin/python" \
    "${HOME}/.platformio/python3/bin/python3"; do
    if [[ -x "${candidate}" ]]; then
        PIO_PENV_PY="${candidate}"
        break
    fi
done

# --- 2. If `pio` is on PATH, extract its python from the shebang -----------
if [[ -z "${PIO_PENV_PY}" ]] && command -v pio >/dev/null 2>&1; then
    PIO_BIN="$(command -v pio)"
    FIRST_LINE="$(head -n1 "${PIO_BIN}" 2>/dev/null || true)"
    if [[ "${FIRST_LINE}" == \#\!* ]]; then
        CAND="${FIRST_LINE#\#\!}"
        CAND="${CAND%% *}" # strip any args after the interpreter
        if [[ -x "${CAND}" ]]; then
            PIO_PENV_PY="${CAND}"
        fi
    fi
fi

ESPTOOL_CMD=()
if [[ -n "${PIO_PENV_PY}" && -f "${ESPTOOL_PY}" ]] \
   && "${PIO_PENV_PY}" -c "import serial" >/dev/null 2>&1; then
    ESPTOOL_CMD=("${PIO_PENV_PY}" "${ESPTOOL_PY}")
else
    # --- 3. Fall back to system python that has esptool installed ---------
    SYS_PYTHON="$(command -v python3 || command -v python || true)"
    if [[ -z "${SYS_PYTHON}" ]]; then
        echo "ERROR: no python interpreter found on PATH" >&2
        exit 1
    fi
    if "${SYS_PYTHON}" -c "import esptool, serial" >/dev/null 2>&1; then
        ESPTOOL_CMD=("${SYS_PYTHON}" "-m" "esptool")
    else
        echo "ERROR: esptool + pyserial not importable from ${SYS_PYTHON}." >&2
        echo "       Install with:" >&2
        echo "         ${SYS_PYTHON} -m pip install --break-system-packages --user esptool pyserial" >&2
        exit 1
    fi
fi

"${ESPTOOL_CMD[@]}" --chip esp32s3 merge_bin \
    -o "${OUT_DIR}/esp32-boat-${TAG}-merged.bin" \
    --flash_mode qio --flash_freq 80m --flash_size 16MB \
    0x0000  "${OUT_DIR}/esp32-boat-${TAG}-bootloader.bin" \
    0x8000  "${OUT_DIR}/esp32-boat-${TAG}-partitions.bin" \
    0xe000  "${OUT_DIR}/esp32-boat-${TAG}-boot_app0.bin" \
    0x10000 "${OUT_DIR}/esp32-boat-${TAG}-firmware.bin"

echo
echo "==> Done. Binaries in: ${OUT_DIR}"
ls -l "${OUT_DIR}" | grep "esp32-boat-${TAG}"
echo
echo "To flash the merged image to a board plugged into /dev/cu.usbmodemXXXX:"
echo "  python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \\"
echo "      write_flash 0x0 ${OUT_DIR}/esp32-boat-${TAG}-merged.bin"
