#!/usr/bin/env bash
# flash.sh — build, flash, and monitor either the transmitter or the receiver.
#
# We have two boards, both ESP32-S3, both showing up as /dev/cu.usbmodem*
# on macOS. The OS gives no hint which is which. Flashing the receiver
# firmware onto the transmitter (or vice-versa) would overwrite a working
# image with the wrong code — possibly bricking the user-facing display
# until restored from backup. Not catastrophic, but easy to avoid.
#
# This script prevents that by reading the connected chip's MAC at the
# start of every run. The MAC is registered on first use (per role), then
# verified on every subsequent run. If you connect the wrong board, the
# script aborts before any bytes hit flash.
#
# Usage:
#   ./scripts/flash.sh tx                  Flash + monitor the transmitter
#   ./scripts/flash.sh rx                  Flash + monitor the receiver
#   ./scripts/flash.sh tx --no-monitor     Flash only, skip monitor
#   ./scripts/flash.sh tx --register       Register the connected board's
#                                          MAC as the transmitter (use when
#                                          adding a new/replacement board,
#                                          or when overwriting an old MAC)
#
# Roles → PIO envs:
#   tx → [env:nmea2k_tx]                       — ESP32-S3-Touch-AMOLED-1.75-G
#   rx → [env:waveshare_esp32s3_touch_lcd_21_ble]
#                                              — Waveshare 2.1" round display.
#                                                Step 4 (May 12 2026) switched
#                                                this from the sim env to the
#                                                BLE-client env. The RX scans
#                                                for esp32-boat-tx and feeds
#                                                BoatState from BLE notifies.
#
# Storage:
#   scripts/.devices.conf — newline-separated `role=AA:BB:CC:DD:EE:FF`
#   entries. Gitignored, so each clone of the repo registers its own boards.
#
# Exit codes:
#   0 success
#   1 usage error / wrong board / build failed / user aborted
#   2 no USB device / esptool unavailable / MAC read failed

set -euo pipefail

# ---- parse args -------------------------------------------------------------
ROLE="${1:-}"
shift || true

MONITOR=true
REGISTER=false
for arg in "$@"; do
    case "$arg" in
        --no-monitor) MONITOR=false ;;
        --register)   REGISTER=true ;;
        -h|--help)
            sed -n '2,40p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown flag: $arg" >&2
            echo "Run with --help for usage." >&2
            exit 1
            ;;
    esac
done

case "$ROLE" in
    tx) ENV="nmea2k_tx" ;;
    # Step 4 (May 12 2026): RX defaults to the BLE-client env that
    # consumes from esp32-boat-tx. The sim env is still in platformio.ini
    # if you ever need to bench-test the UI without a TX nearby — flash
    # it directly with: pio run -e waveshare_esp32s3_touch_lcd_21_sim -t upload
    rx) ENV="waveshare_esp32s3_touch_lcd_21_ble" ;;
    "")
        echo "Usage: $0 <tx|rx> [--no-monitor] [--register]" >&2
        echo "       $0 --help" >&2
        exit 1
        ;;
    *)
        echo "Unknown role: $ROLE (must be 'tx' or 'rx')" >&2
        exit 1
        ;;
esac

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEVICES="$REPO_ROOT/scripts/.devices.conf"

# ---- 1. find the device ----------------------------------------------------
# macOS exposes the ESP32-S3's native USB-CDC as /dev/cu.usbmodem*. If the
# board isn't plugged in, or the cable is power-only, this glob returns
# nothing and shell expands to the literal pattern.
PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)"
if [[ -z "$PORT" ]]; then
    echo "ERROR: No /dev/cu.usbmodem* device found." >&2
    echo "       Plug in the $ROLE board via USB-C and try again." >&2
    echo "       (Some Type-C cables are charge-only — try a different cable" >&2
    echo "       if the board is powered but doesn't enumerate.)" >&2
    exit 2
fi
echo "==> Port: $PORT"

# ---- 2. read the chip MAC --------------------------------------------------
# We try `esptool` on PATH first. PlatformIO installs its own copy under
# ~/.platformio/penv/bin/esptool, so fall back there if the user's PATH is
# clean. Either works; we just need *some* esptool to introspect the chip.
ESPTOOL=""
if command -v esptool >/dev/null 2>&1; then
    ESPTOOL="esptool"
elif [[ -x "$HOME/.platformio/penv/bin/esptool" ]]; then
    ESPTOOL="$HOME/.platformio/penv/bin/esptool"
else
    echo "ERROR: 'esptool' not found." >&2
    echo "       Install via 'pip install esptool' or run a PIO build first" >&2
    echo "       (which auto-installs it to ~/.platformio/penv/bin/esptool)." >&2
    exit 2
fi

# `read-mac` prints "MAC: aa:bb:cc:dd:ee:ff" on a line of its own. Newer
# esptool also dumps base-MAC for ESP32-S3; we grab the first MAC: line.
MAC="$("$ESPTOOL" --chip esp32s3 --port "$PORT" read-mac 2>&1 \
       | awk '/^MAC:/ { print $2; exit }' || true)"
if [[ -z "$MAC" ]]; then
    echo "ERROR: Could not read MAC from $PORT." >&2
    echo "       Possible causes:" >&2
    echo "         - Device is in download mode but esptool can't see it" >&2
    echo "           (try unplugging + replugging without holding BOOT)." >&2
    echo "         - Cable is power-only or flaky." >&2
    echo "         - Another process has the port open (close screen/tio)." >&2
    exit 2
fi
echo "==> Chip MAC: $MAC"

# ---- 3. compare MAC against the registered one for this role ---------------
mkdir -p "$(dirname "$DEVICES")"
touch "$DEVICES"
REGISTERED="$(grep -E "^${ROLE}=" "$DEVICES" 2>/dev/null \
              | head -n1 | cut -d= -f2- || true)"

if $REGISTER; then
    # Explicit register: overwrite whatever's there for this role.
    grep -vE "^${ROLE}=" "$DEVICES" > "$DEVICES.tmp" 2>/dev/null || true
    echo "${ROLE}=${MAC}" >> "$DEVICES.tmp"
    mv "$DEVICES.tmp" "$DEVICES"
    echo "==> Registered $MAC as $ROLE in $DEVICES"
elif [[ -z "$REGISTERED" ]]; then
    echo
    echo "No '$ROLE' device registered yet."
    echo "Register the connected board ($MAC) as $ROLE? [y/N]"
    read -r reply
    if [[ ! "$reply" =~ ^[Yy]$ ]]; then
        echo "Aborted. Re-run with --register to skip this prompt." >&2
        exit 1
    fi
    echo "${ROLE}=${MAC}" >> "$DEVICES"
    echo "==> Registered $MAC as $ROLE in $DEVICES"
elif [[ "$REGISTERED" != "$MAC" ]]; then
    echo
    echo "ABORT: connected MAC ($MAC) is not the registered $ROLE board." >&2
    echo "       Registered $ROLE MAC: $REGISTERED" >&2
    echo
    echo "Either:" >&2
    echo "  - Wrong board plugged in. Disconnect and connect the $ROLE." >&2
    echo "  - Swapping in a replacement: re-run with --register to overwrite." >&2
    exit 1
else
    echo "==> Verified: $MAC is the registered $ROLE device."
fi

# ---- 4. build + flash ------------------------------------------------------
cd "$REPO_ROOT"
echo
echo "==> pio run -e $ENV -t upload --upload-port $PORT"
pio run -e "$ENV" -t upload --upload-port "$PORT"

# ---- 5. monitor ------------------------------------------------------------
if $MONITOR; then
    # After a hard_reset, the chip reboots and USB-CDC re-enumerates. The
    # device file (/dev/cu.usbmodem*) is briefly gone — typically for 1-2
    # seconds on macOS — before reappearing. pio device monitor doesn't
    # retry on its own, so if we open it the instant the flash returns we
    # get "No such file or directory" and the monitor session dies. Poll
    # for the port to come back, then give it a half-second settle margin
    # so HWCDC is fully ready to accept reads before we start.
    echo
    echo "==> waiting for $PORT to re-enumerate ..."
    for _ in $(seq 1 20); do
        if [[ -e "$PORT" ]]; then
            sleep 0.5
            break
        fi
        sleep 0.25
    done
    if [[ ! -e "$PORT" ]]; then
        echo "WARNING: $PORT didn't come back within 5 s; trying anyway." >&2
    fi

    echo "==> pio device monitor -e $ENV --port $PORT"
    echo "    (Ctrl-C to exit)"
    pio device monitor -e "$ENV" --port "$PORT"
fi
