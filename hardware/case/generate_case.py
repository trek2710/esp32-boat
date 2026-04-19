"""Generate STLs for a 3D-printable round enclosure for the esp32-boat device.

Two parts:
  * case_body.stl  — the main bowl that holds the Waveshare ESP32-S3-Touch-LCD-2.1
    and the SN65HVD230 CAN transceiver breakout, with a USB-C slot on the side
    and a cable exit hole for the NMEA 2000 wire.
  * case_bezel.stl — a flat ring with the display window that press-fits into
    the top of the body, sandwiching the PCB against the internal shelf.

All dimensions are parametric up top. Adjust & re-run if the hardware you get
measures different to the nominals I've used.

Print notes
-----------
Both parts print open-end / skirt pointing UP with no supports in most slicers.
The horizontal cable hole and USB-C slot on the body's side wall may benefit
from a few support lines if your printer's bridging is weak.
"""

import numpy as np
import manifold3d as m3d
from stl import mesh as stl_mesh

# Smoother circles — 96 segments on an Ø75 cylinder = ~2.5mm arc per segment.
m3d.set_circular_segments(96)


# =============================================================================
# Parameters — edit these if your hardware measures differently.
# =============================================================================

# Waveshare ESP32-S3-Touch-LCD-2.1 (round 2.1" panel):
PCB_DIA            = 65.0   # PCB outer diameter
PCB_THK            = 11.0   # PCB + components total depth
DISPLAY_VIS_DIA    = 53.4   # active display diameter (2.1")

# SN65HVD230 transceiver breakout (tiny — this is generous):
# No direct constraints; the "transceiver bay" just needs to be roomy.

# Case geometry:
BODY_OD            = 75.0   # outer diameter
WALL_MIN           = 2.5    # minimum wall thickness anywhere

BACK_THK           = 2.0    # solid back wall thickness
TRANS_BAY_DIA      = 60.0   # inner diameter of transceiver bay
TRANS_BAY_H        = 6.0    # transceiver bay depth
PCB_CAVITY_DIA     = 66.5   # PCB cavity — 1.5mm bigger than PCB for slop
PCB_CAVITY_H       = PCB_THK
TOP_RIM_H          = 2.0    # rim above PCB where the bezel skirt sits

# Total body height is the sum of those four:
BODY_H             = BACK_THK + TRANS_BAY_H + PCB_CAVITY_H + TOP_RIM_H

# USB-C cutout (+X side wall):
USB_CUTOUT_W       = 12.0
USB_CUTOUT_H       = 6.0
# Centered on the USB-C connector height — connector is typically on the
# back side of the PCB (toward the case interior), mid-thickness.
USB_CUTOUT_Z       = BACK_THK + TRANS_BAY_H + PCB_THK * 0.45

# Cable exit hole (-X side wall, in the transceiver bay):
CABLE_HOLE_DIA     = 6.0    # fits a 5mm-OD cable + strain relief / grommet
CABLE_HOLE_Z       = BACK_THK + TRANS_BAY_H * 0.5

# Bezel:
BEZEL_OD           = BODY_OD
BEZEL_FLAT_THK     = 2.0
BEZEL_SKIRT_OD     = PCB_CAVITY_DIA - 0.3   # ~0.15mm radial clearance → press fit
BEZEL_SKIRT_H      = TOP_RIM_H              # fills the rim exactly
BEZEL_WINDOW_DIA   = DISPLAY_VIS_DIA + 2.5  # 1.25mm margin on each side


# =============================================================================
# Helpers
# =============================================================================

def manifold_to_stl(manifold, filename):
    """Dump a manifold3d Manifold to a binary STL via numpy-stl."""
    mesh = manifold.to_mesh()
    verts = np.asarray(mesh.vert_properties, dtype=np.float32)[:, :3]
    tris  = np.asarray(mesh.tri_verts, dtype=np.uint32)
    data = np.zeros(len(tris), dtype=stl_mesh.Mesh.dtype)
    data['vectors'] = verts[tris]
    m = stl_mesh.Mesh(data)
    m.save(filename)
    print(f"  wrote {filename}  ({len(tris)} triangles, "
          f"{manifold.volume()/1000:.2f} cm^3)")


# =============================================================================
# Body
# =============================================================================

def build_body():
    # Start with a solid cylinder for the outer shell.
    body = m3d.Manifold.cylinder(BODY_H, BODY_OD / 2)

    # Subtract the transceiver bay (Ø60 × 6mm, just above the back wall).
    z0 = BACK_THK
    trans_cav = (m3d.Manifold.cylinder(TRANS_BAY_H + 0.01, TRANS_BAY_DIA / 2)
                 .translate([0, 0, z0]))
    body = body - trans_cav

    # Subtract the PCB cavity (Ø66.5 × 11mm) AND the top rim clearance
    # (same inner diameter extending through the top). One cylinder does both.
    z1 = z0 + TRANS_BAY_H
    pcb_cav = (m3d.Manifold.cylinder(PCB_CAVITY_H + TOP_RIM_H + 0.01,
                                     PCB_CAVITY_DIA / 2)
               .translate([0, 0, z1]))
    body = body - pcb_cav

    # USB-C cutout through the +X wall at PCB-edge height.
    usb = (m3d.Manifold.cube([BODY_OD, USB_CUTOUT_W, USB_CUTOUT_H])
           .translate([0,                        # cube centred on X=0 origin
                       -USB_CUTOUT_W / 2,
                       USB_CUTOUT_Z - USB_CUTOUT_H / 2]))
    # Shift cube so its -X face is at X = 0 (ie it extends from origin to +BODY_OD
    # along X). With the centred body (also at origin), the cube passes through
    # the +X wall only.
    # Actually manifold3d cube() places the corner at origin by default —
    # check: yes, Manifold.cube([w,d,h]) has corner at origin and extends to +w.
    body = body - usb

    # Cable exit hole through the -X wall (horizontal Ø6 cylinder).
    # Default cylinder axis is +Z; rotate 90° about Y to make axis +X.
    cable = (m3d.Manifold.cylinder(BODY_OD + 10, CABLE_HOLE_DIA / 2)
             .rotate([0, 90, 0])
             .translate([-BODY_OD / 2 - 5, 0, CABLE_HOLE_Z]))
    body = body - cable

    return body


# =============================================================================
# Bezel
# =============================================================================

def build_bezel():
    # Flat ring (OD 75 × 2mm) with a smaller Ø skirt on top.
    flat  = m3d.Manifold.cylinder(BEZEL_FLAT_THK,  BEZEL_OD / 2)
    skirt = (m3d.Manifold.cylinder(BEZEL_SKIRT_H, BEZEL_SKIRT_OD / 2)
             .translate([0, 0, BEZEL_FLAT_THK]))
    bezel = flat + skirt

    # Drill the display window all the way through.
    total_h = BEZEL_FLAT_THK + BEZEL_SKIRT_H
    window  = (m3d.Manifold.cylinder(total_h + 0.2, BEZEL_WINDOW_DIA / 2)
               .translate([0, 0, -0.1]))
    bezel = bezel - window

    return bezel


# =============================================================================
# Main
# =============================================================================

if __name__ == "__main__":
    print(f"Building body  (Ø{BODY_OD} × {BODY_H} mm)")
    body = build_body()
    manifold_to_stl(body, "case_body.stl")

    print(f"Building bezel (Ø{BEZEL_OD} × {BEZEL_FLAT_THK + BEZEL_SKIRT_H} mm)")
    bezel = build_bezel()
    manifold_to_stl(bezel, "case_bezel.stl")

    print()
    print("Assembly:")
    print(f"  1. Drop SN65HVD230 breakout into the transceiver bay at the back.")
    print(f"  2. Route the CAN wires through the Ø{CABLE_HOLE_DIA}mm exit hole.")
    print(f"  3. Insert the Waveshare PCB into the PCB cavity, display forward;")
    print(f"     back edge should rest on the internal shelf.")
    print(f"  4. Press the bezel onto the top; skirt slides 2mm into the rim.")
    print(f"  5. USB-C accessible through the side slot at "
          f"z={USB_CUTOUT_Z:.1f} mm.")
