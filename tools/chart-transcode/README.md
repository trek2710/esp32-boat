# Chart transcode (CM93 → ESP32/iOS tiles)

One-off, Mac-side pipeline that decodes the CM93 charts once and bakes a
compact vector format the AIS-radar device (and the iOS app) can render
directly — **no chart decoder on the MCU, no BLE chart streaming.** Decided
in the v2 map-overlay design; the device card is in the ESP32, so the heavy
decode happens here on the computer where we have real tools.

## Status — CM93 decode spike: SUCCESS ✅

`cm93_decode.py` is a working CM93 v2 cell decoder (ported from OpenCPN's
`cm93.cpp`, GPLv2 — kept local under the gitignored `reference/`). Run:

```
python3 cm93_decode.py [chart_root] [lat] [lon] [scale]
# default: .../cm93_World2014 55.68 12.57 C  (Copenhagen, coastal scale)
```

On the real Copenhagen cell `03900000/C/04260036.C` it:
- passed the cell integrity checksum and **parsed to the exact byte**
  (279961/279961) across 3341 features — strong proof the grammar is right;
- decoded **321 coastline polylines / 13 698 points** that plot as real
  Øresund/Baltic shoreline (`spike_out.geojson`);
- confirmed **all five target layers** exist as object classes:
  `COALNE` (coastline), `DEPARE`+`DEPCNT` (depth areas/contours),
  `TSSLPT` (traffic separation), `BOYLAT`/`BOYCAR` (buoys), `LIGHTS`.

→ Decision: build the transcoder on CM93 (fully offline, the user's own
charts). Open-data fallback not needed.

## Format notes (so we never re-derive this)

- Every byte is de-obfuscated through a 256-entry substitution
  (`DECODE_TABLE`, == inverse of `Table_0[i]^8`). It's a pure per-byte map,
  so we de-obfuscate the whole file once, then parse with a cursor.
- Cell file = 10-byte prolog (`u16` prolog+header len = 138, `i32` table-1
  len, `i32` table-2 len; the three sum to the file length) → 128-byte
  header (8 `double` bbox: lon/lat min/max then Mercator E/N min/max; then
  record counts) → table 1 (edge/vector records, point3d, point2d) →
  table 2 (feature records). All little-endian.
- Geometry points are `u16` (x,y) mapped linearly across the cell, then
  inverse spherical Mercator on the International-1924 axis (a=6378388 m).
- Feature record: `u8` class, `u8` geom-prim+flags, `u16` desc-len, then
  geometry (prim&0x0f: 4=area,2=line via edge refs `idx&0x1fff`;
  1=2d pt, 8=3d pt), optional related objects (flags 0x10/0x20), optional
  attribute block (flag 0x80).
- Dictionary ships with the charts: `CM93OBJ.DIC` (`NAME|classnum|geom`),
  `CM93ATTR.DIC` (attribute names/types — needed for `DRVAL1` depth values).
- Cell path: `root(ilatroot⁴ilonroot⁴)/<scale Z–G>/?<jlat³><jlon⁴>.<scale>`.

## Next (not yet built)

1. Attribute decode (`DRVAL1`/`DRVAL2` for the 3 m depth split; light/buoy
   `COLOUR`/`category`).
2. Clip to the Copenhagen box, simplify (Douglas–Peucker), emit a compact
   tiled binary: coastline + depth(<3 m / ≥3 m) + TSS lines + buoy/light
   points, per layer.
3. ESP32 reader + renderer (overlay under the radar); per-layer toggles via
   the existing BLE settings characteristic.
4. Same tiles bundled into the iOS app; same renderer + toggles.
