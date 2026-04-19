# Pre-built firmware

This folder holds ready-to-flash binaries for the `esp32-boat` device so you
can put firmware on the board without installing PlatformIO or a C++
toolchain.

## What's here

Each build produces a set of `.bin` files tagged with its env:

| File | Env tag | Flash offset | Notes |
|---|---|---|---|
| `esp32-boat-<tag>-bootloader.bin` | sim / prod | `0x0000` | Second-stage bootloader |
| `esp32-boat-<tag>-partitions.bin` | sim / prod | `0x8000` | Partition table (16 MB) |
| `esp32-boat-<tag>-boot_app0.bin`  | sim / prod | `0xe000` | Arduino OTA selector |
| `esp32-boat-<tag>-firmware.bin`   | sim / prod | `0x10000` | The app itself |
| `esp32-boat-<tag>-merged.bin`     | sim / prod | `0x0000` | All of the above in one file |

Tags:

- **sim** — simulated data, no CAN bus required. Use this for bench testing
  the UI (swipe between Overview / Data / Debug screens with fake values).
- **prod** — talks to the real NMEA 2000 bus via the SN65HVD230 transceiver
  wired to `CAN_TX_PIN` / `CAN_RX_PIN` (see `src/config.h`).

## Target hardware

- **Board**: Waveshare ESP32-S3-Touch-LCD-2.1 (any ESP32-S3 devkit works for
  the sim build if you just want to exercise the firmware).
- **Chip**: ESP32-S3
- **Flash size**: 16 MB, QIO @ 80 MHz
- **USB**: native USB-CDC (no external USB-serial chip needed)

## Flashing — option 1: web browser (easiest)

1. Plug the ESP32-S3 into your computer's USB port.
2. Open <https://espressif.github.io/esptool-js/> in Chrome or Edge.
   *(Firefox and Safari don't support the WebSerial API yet.)*
3. Click **Connect** and pick the `/dev/cu.usbmodem…` (macOS/Linux) or
   `COMx` (Windows) port that belongs to the board.
4. Under **Flash Address**, set `0x0`, click **Choose a file** and pick
   `esp32-boat-sim-merged.bin` (or `esp32-boat-prod-merged.bin`).
5. Click **Program**. When it finishes, open the **Reset** button or
   unplug/replug to reboot into the new firmware.

## Flashing — option 2: esptool.py (command line)

esptool is a small Python CLI; install it with `pip` if you don't already
have it:

```
python3 -m pip install --user esptool
```

Plug in the board, figure out the port:

```
# macOS / Linux
ls /dev/cu.usbmodem*     # macOS
ls /dev/ttyUSB* /dev/ttyACM*   # Linux

# Windows
# Device Manager → Ports (COM & LPT) → look for "USB Serial Device (COMx)"
```

Then flash the merged image:

```
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
    write_flash 0x0 esp32-boat-sim-merged.bin
```

Open the serial monitor at 115200 baud to see log output:

```
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX read_mac    # sanity check
# any serial terminal works — screen, picocom, minicom, Arduino monitor, PlatformIO
```

If the chip can't be detected, hold the **BOOT** button on the devkit,
briefly tap **RESET**, then release **BOOT** — this forces bootloader
mode. Most S3 boards don't need this for a first flash, but some do.

### Flashing the individual files instead of the merged one

If you'd rather flash the four parts directly (e.g. you're experimenting
with a different partition table):

```
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
    write_flash \
    0x0000  esp32-boat-sim-bootloader.bin \
    0x8000  esp32-boat-sim-partitions.bin \
    0xe000  esp32-boat-sim-boot_app0.bin \
    0x10000 esp32-boat-sim-firmware.bin
```

## Flashing — option 3: PlatformIO (for developers)

If you already have the toolchain:

```
pio run -e waveshare_esp32s3_touch_lcd_21_sim -t upload \
    --upload-port /dev/cu.usbmodemXXXX
```

PlatformIO will build + flash in one step; no need for the committed
binaries at all.

## Rebuilding these binaries

See `scripts/package.sh` at the repo root. Run it from the project root on
a machine with PlatformIO installed:

```
./scripts/package.sh sim    # builds the sim firmware, populates binaries/
./scripts/package.sh prod   # production build (requires an S3-capable CAN backend)
```

Then commit the updated `.bin` files alongside your source changes so the
folder stays in sync with the code.

## Notes & known limitations

- The firmware currently uses a **stub display driver**. On the Waveshare
  2.1" round panel the screen will stay dark until the ST77916 QSPI driver
  is wired up. Serial output still works — the three on-screen pages log
  their boot sequence, so you can verify a successful flash.
- The **production env** (`waveshare_esp32s3_touch_lcd_21`) does not
  currently compile on ESP32-S3 because the pinned
  `NMEA2000_esp32@^1.0.3` uses ESP32-classic CAN registers. This will be
  resolved by switching to a S3-capable backend (`NMEA2000_esp32xx` or a
  thin TWAI driver).
- The `.bin` files in this folder are what was built at the time of the
  last `scripts/package.sh` run — check `git log` on this directory to see
  when the snapshot is from.
