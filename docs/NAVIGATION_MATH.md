# Navigation math: what's simulated, what's derived

This document is the source-of-truth for which numbers on the dial come
straight from a sensor and which the device computes for itself. It also
walks through the formulas used in `BoatState::recomputeDerived_locked`
and `BoatState::setGps`, and finishes with three end-to-end worked
examples plus a diagram of the wind triangle.

The same model lives in code at `src/BoatState.cpp` (derive logic) and
`src/NmeaBridge.cpp` (which sensor outputs map onto which raw fields,
plus the simulator that fakes them on the bench).

---

## 1. What sensors actually publish

These are the *only* values the device accepts as raw input — both from
the real NMEA 2000 bus and from the SIMULATED_DATA build:

| Field                         | Source on a real boat                       | NMEA 2000 PGN |
|-------------------------------|---------------------------------------------|--------------|
| `lat`, `lon`                  | GPS receiver — position fix                 | 129025       |
| `awa`                         | Masthead anemometer — apparent wind ANGLE   | 130306 (apparent ref) |
| `aws`                         | Masthead anemometer — apparent wind SPEED   | 130306 (apparent ref) |
| `heading_mag_deg`             | Fluxgate / electronic compass (magnetic)    | 127250       |
| `magnetic_variation_deg`      | World Magnetic Model lookup (or PGN field)  | 127250 / WMM table |
| `stw`                         | Paddlewheel or ultrasonic log               | 128259       |
| `depth_m`, `water_temp_c`     | Depth sounder + temperature probe           | 128267 / 130316 |

That's it. **Boat heading comes from the COMPASS**, not from GPS.
**Movement direction comes from a GPS-position differential**, not from
a sensor-supplied COG/SOG. The real bridge ignores PGN 129026 (the
sensor-supplied COG/SOG message) so we never double-source these
quantities and so a sensor dropout immediately surfaces.

The SIMULATED_DATA build emits *only* this same set: it picks a
slowly-varying intended speed-and-heading-over-ground each tick and
**integrates** the boat's position from there, so SOG/COG have to come
out of the deltas.

## 2. What the device derives

Everything else on the dial is derived inside `BoatState`. Each setter
above runs `recomputeDerived_locked()` after writing its raw field, so
the snapshot is always coherent.

### 2.1 SOG and COG (from consecutive GPS fixes)

Done in `BoatState::setGps(lat, lon)`. Equirectangular plate-carrée
approximation — accurate to better than 0.1 % on the < 100 m baselines
between 1 Hz fixes at boat speeds, and avoids the trigonometry of a
full great-circle solution:

```
    dx_m       = (lon₂ − lon₁) · R · cos(mid_lat)
    dy_m       = (lat₂ − lat₁) · R
    distance_m = √(dx_m² + dy_m²)
    SOG_kn     = (distance_m / Δt_s) · (3600 / 1852)
    COG_deg    = (atan2(dx_m, dy_m) · 180/π)  mod 360
```

with `R = 6 371 008.8 m`. `mid_lat` is in radians and is the average of
the two fix latitudes. We use `atan2(dx, dy)` (NOT `atan2(dy, dx)`) so
the bearing is measured east-of-north — the sailor's convention.

If the boat moved less than 0.5 m between fixes, `COG` is left at its
previous value (otherwise GPS jitter at the metre scale would spin it
wildly). `SOG` is always recomputed.

### 2.2 True heading (magnetic + variation)

```
    heading_true_deg = (heading_mag_deg + magnetic_variation_deg) mod 360
```

Magnetic variation (declination) is positive east. It comes from
`navmath::lookupMagneticVariation(lat, lon)` — currently a stub that
returns Copenhagen's ≈ +5° east; will load the World Magnetic Model
coefficient grid from the SD card in a later round.

If either input is `NaN`, `heading_true` is `NaN` and the UI shows
`---`.

### 2.3 True wind (apparent wind + boat motion)

The classic vector subtraction. Convention: angles are measured FROM
where the wind comes (sailor's convention, so AWA = 0 means a wind
from straight ahead). Boat-relative frame: bow = +y, starboard = +x.

```
    apparent wind velocity vector  V_a = -AWS · (sin AWA, cos AWA)
                                          (negated because AWA is the
                                           direction the wind comes FROM,
                                           opposite to the air's motion)
    boat velocity vector           V_b = (0, STW)
    true wind velocity vector      V_t = V_a + V_b
```

The "from" angle (TWA) is `atan2(-V_t.x, -V_t.y)`. Algebra collapses
to:

```
    TWA = atan2( AWS · sin AWA,  AWS · cos AWA − STW )
    TWS =  √( (AWS · sin AWA)² + (AWS · cos AWA − STW)² )
```

If STW is missing, the code falls back to SOG. (In slack water the two
are equal; in current the wind triangle is then off by the current
vector — fine for v1, will revisit if/when current sensing lands.)

### 2.4 True wind direction (TWD) and VMG

```
    TWD  = (heading_true_deg + TWA) mod 360
    VMG  = STW · cos(TWA · π/180)
```

Negative VMG = sailing downwind.

---

## 3. The wind triangle (diagram)

See [docs/wind-triangle.svg](wind-triangle.svg) for the vector picture
that the formulas above implement. The diagram has the bow pointing up,
the boat-velocity vector along +y, the apparent-wind vector at the
masthead, and the true-wind vector closing the triangle.

---

## 4. Three worked examples

Each example walks all the way from raw sensor inputs to the dial
values the user sees. Plug the inputs into `BoatState` and you should
get the derived numbers shown.

### Example 1 — calm light air, beating upwind

A 3.5 kn breeze coming from a little to the right of the bow, boat
making 2 kn through the water heading roughly 020° true.

| Raw input                  | Value          |
|----------------------------|----------------|
| `heading_mag_deg`          |   015°         |
| `magnetic_variation_deg`   |   +5° (east)   |
| `awa`                      |   +30° (stb)   |
| `aws`                      |   3.5 kn       |
| `stw`                      |   2.0 kn       |

True heading:
```
    heading_true = 015 + 5 = 020°
```

True wind:
```
    AWS · sin AWA = 3.5 · sin 30° = 1.75
    AWS · cos AWA = 3.5 · cos 30° = 3.031
    TWA  = atan2( 1.75,  3.031 − 2.0 )
         = atan2( 1.75,  1.031 )
         ≈ 59.5° starboard
    TWS  = √(1.75² + 1.031²) ≈ √(3.063 + 1.063) ≈ 2.03 kn
```

True wind direction:
```
    TWD  = (020 + 59.5) mod 360  = 079.5° true
```

VMG:
```
    VMG  = 2.0 · cos 59.5°  ≈ 2.0 · 0.508  ≈ 1.02 kn
```

So the dial shows: HDG 020°T, TWA +60°, TWS 2.0 kn, TWD 080°T, VMG +1.0 kn.
You're on a beat at about 60° off the true wind, picking up about 1 kn
toward the mark.

### Example 2 — 15 kn beam reach

A solid sea breeze on the beam, boat overpowered slightly heading 270°
true with 6 kn through the water.

| Raw input                  | Value          |
|----------------------------|----------------|
| `heading_mag_deg`          |   265°         |
| `magnetic_variation_deg`   |   +5°          |
| `awa`                      |   +60° (stb)   |
| `aws`                      |  18.0 kn       |
| `stw`                      |   6.0 kn       |

True heading:
```
    heading_true = 265 + 5 = 270°
```

True wind:
```
    AWS · sin AWA = 18 · sin 60° = 15.59
    AWS · cos AWA = 18 · cos 60° =  9.00
    TWA = atan2(15.59, 9.00 − 6.0) = atan2(15.59, 3.00)
        ≈ 79.1° starboard
    TWS = √(15.59² + 3.00²) = √(243.0 + 9.0) ≈ 15.87 kn
```

```
    TWD = (270 + 79.1) mod 360 = 349.1° true
    VMG = 6.0 · cos 79.1° ≈ 6.0 · 0.189 ≈ 1.13 kn
```

Dial: HDG 270°T, TWA +79°, TWS 15.9 kn, TWD 349°T, VMG +1.1 kn.
Classic close reach — apparent wind bowed forward of the true.

### Example 3 — 25 kn broad reach with current

Strong following breeze on a downwind run heading 195° true at 7 kn STW.

| Raw input                  | Value          |
|----------------------------|----------------|
| `heading_mag_deg`          |   190°         |
| `magnetic_variation_deg`   |   +5°          |
| `awa`                      |  +130° (stb)   |
| `aws`                      |  19.0 kn       |
| `stw`                      |   7.0 kn       |

True heading:
```
    heading_true = 190 + 5 = 195°
```

True wind:
```
    AWS · sin AWA = 19 · sin 130° = 19 · 0.766 = 14.55
    AWS · cos AWA = 19 · cos 130° = 19 · (−0.643) = −12.21
    TWA = atan2(14.55, −12.21 − 7.0) = atan2(14.55, −19.21)
        ≈ 180° − 37.2° = 142.8° starboard
    TWS = √(14.55² + 19.21²) = √(211.7 + 369.0) ≈ 24.10 kn
```

```
    TWD = (195 + 142.8) mod 360 = 337.8° true
    VMG = 7.0 · cos 142.8° ≈ 7.0 · (−0.797) ≈ −5.58 kn
```

Dial: HDG 195°T, TWA +143°, TWS 24.1 kn, TWD 338°T, VMG −5.6 kn.
The negative VMG (sailing 5.6 kn AWAY from the wind) is exactly what
you want on a downwind run.

---

## 5. Cross-references

| Document concept                          | Code reference                                     |
|-------------------------------------------|----------------------------------------------------|
| Raw vs derived split                      | `src/BoatState.h` Instruments struct comments      |
| `recomputeDerived_locked` derive pipeline | `src/BoatState.cpp`                                |
| SOG/COG from GPS deltas                   | `BoatState::setGps()` in `src/BoatState.cpp`       |
| Magnetic variation lookup                 | `src/magnetic_variation.{h,cpp}`                   |
| Sim integrating a real trajectory         | `NmeaBridge::simulateTick()` in `src/NmeaBridge.cpp` |
| Real bridge → BoatState mapping           | `NmeaBridge::handleMsg` PGN handlers in `NmeaBridge.cpp` |
