# 3D-printable enclosure

Two-part round case for the Waveshare ESP32-S3-Touch-LCD-2.1 + SN65HVD230
transceiver. Designed around the nominal dimensions of both boards — re-run
`generate_case.py` with different parameters if your hardware measures
differently.

## Files

| File | What it is |
|---|---|
| `case_body.stl` | The main bowl. Holds the transceiver at the back, the Waveshare PCB in front of it. |
| `case_bezel.stl` | Flat ring with the display window. Press-fits into the top of the body. |
| `generate_case.py` | Parametric source that produced the STLs. All dimensions are constants at the top of the file. |

## Dimensions

- Outer diameter: **Ø75 mm**
- Total height (body + bezel): **25 mm** (21 mm body + 4 mm bezel, ~2 mm overlap)
- Display window: **Ø55.9 mm** (clears the 2.1" / Ø53.4 mm active area)
- USB-C slot: 12 × 6 mm on the +X side wall, centred at z ≈ 12.9 mm
- NMEA 2000 cable exit: Ø6 mm hole through the -X side wall, in the transceiver bay

The inside has an **internal shelf** that the PCB rests on, so the display
sits flush with the bezel opening.

## Print settings

- **Material**: PETG (marine environment, UV & humidity) or PLA for bench use.
- **Orientation**:
  - *Body* — open end up, back wall on the build plate. No supports needed;
    slicers will bridge the internal shelf and side holes fine at ≤6 mm.
  - *Bezel* — flat side down. No supports.
- **Layer height**: 0.2 mm. 0.16 mm if you want the tick marks around the
  display to look nice.
- **Walls**: 3 perimeters (≥1.2 mm) — the wall-min is 2.5 mm everywhere so
  even default settings give you solid walls.
- **Infill**: 20 % is plenty; the case is tiny.
- **Support**: only needed if your printer bridges the horizontal USB-C slot
  and cable hole badly. Try without first.

## Assembly

1. Drop the **SN65HVD230** breakout into the Ø60 mm transceiver bay at the
   back. Secure with a dab of hot glue or double-sided tape (there's a
   generous 6 mm of depth to play with).
2. Route the CAN twisted pair + 12 V / GND through the **Ø6 mm cable exit
   hole** in the side wall. A small rubber grommet or a dab of silicone
   keeps water out.
3. Insert the **Waveshare ESP32-S3-Touch-LCD-2.1** into the PCB cavity,
   display facing up. The back of the PCB rests on the internal shelf; the
   cavity is 1.5 mm oversize so the board slides in with a little slop.
4. Verify **USB-C** on the Waveshare board lines up with the side slot.
   If it doesn't, rotate the PCB 90°/180° — the USB connector's exact
   clock position is defined by how you orient the PCB inside the round
   cavity. (If the slot is off-axis when the PCB is aligned the way you
   want for the screen, re-run `generate_case.py` with `USB_CUTOUT_Z`
   adjusted.)
5. Press the **bezel** onto the top. The skirt (Ø66.2 mm) slides 2 mm into
   the top rim (Ø66.5 mm) — light interference fit. Push firmly until the
   bezel flat meets the top of the body.

If you ever need to get back in, a thin knife or guitar pick in the seam
between bezel and body will pop it off without damage.

## Regenerating the STLs

You need `manifold3d` and `numpy-stl`. On macOS with Homebrew Python:

```
pip3 install --break-system-packages manifold3d numpy-stl
```

Then:

```
cd hardware/case
python3 generate_case.py
```

That rewrites `case_body.stl` and `case_bezel.stl` next to the script.
Edit the constants at the top of the `.py` file to change dimensions.

## TODO for a future revision

- Mounting tabs or magnets for attaching to a bulkhead.
- O-ring groove in the bezel-body seam for full IP65.
- Recess on the back for a wall-mount screw or VESA pattern.
- Light pipe / gasket around the display edge for a cleaner look.
