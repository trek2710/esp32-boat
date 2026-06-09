#!/usr/bin/env python3
"""Stage B — pack the intermediate into compact per-cell device tiles (.c93t).

Layer-tagged so the SAME file serves device + phone; the renderer filters by
layer toggle and (for depth) a runtime threshold, so DRVAL1 is kept per
polygon rather than baked. Lines/areas are Douglas–Peucker simplified to a
display-matched tolerance.

Tile format (little-endian):
  u32 magic 'C93T'(=0x54333943) | u8 ver=1 | u8 nlayers | u16 pad
  f32 lat_min, lon_min, lat_max, lon_max               # cell bbox
  u32 feat_count
  feat_count × { u8 layer | u8 flags(bit0=area) | f32 depth(NaN if n/a)
                 | u16 npts | npts × (f32 lat, f32 lon) }

Usage: python3 cm93_pack.py [out_dir] [tol_deg]   (default ./tiles  3e-4 ≈ 30 m)
"""
import os, sys, math, struct, pickle

MAGIC = 0x54333943
LAYER = {"COALNE": 0, "LNDARE": 1, "DEPARE": 2, "DEPCNT": 3,
         "TSSLPT": 4, "TSSBND": 4, "TSEZNE": 4, "TSSRON": 4, "ISTZNE": 4,
         "BOYLAT": 5, "BOYCAR": 5, "BOYSAW": 5, "BOYSPP": 5, "BOYINB": 5,
         "BOYISD": 5, "BOYINSTW": 5,
         "LIGHTS": 6}
NLAYERS = 7


def dp(pts, tol, coslat):
    """Douglas–Peucker on (lat,lon); lon scaled by cos(lat) for true distance."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a][1] * coslat, pts[a][0]
        bx, by = pts[b][1] * coslat, pts[b][0]
        dx, dy = bx - ax, by - ay
        seg2 = dx * dx + dy * dy
        dmax, imax = 0.0, -1
        for i in range(a + 1, b):
            px, py = pts[i][1] * coslat, pts[i][0]
            if seg2 == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                t = ((px - ax) * dx + (py - ay) * dy) / seg2
                t = max(0.0, min(1.0, t))
                d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            if d > dmax:
                dmax, imax = d, i
        if dmax > tol and imax > 0:
            keep[imax] = True
            stack.append((a, imax)); stack.append((imax, b))
    return [p for p, k in zip(pts, keep) if k]


def pack_cell(cell, tol):
    la0, lo0, la1, lo1 = cell["bbox"][0], cell["bbox"][1], cell["bbox"][2], cell["bbox"][3]
    coslat = math.cos(math.radians((la0 + la1) / 2))
    body = bytearray()
    n = 0
    for f in cell["features"]:
        L = LAYER.get(f["cls"])
        if L is None:
            continue
        pts = f["points"]
        if f["geom"] in ("line", "area") and len(pts) > 2:
            pts = dp(pts, tol, coslat)
        if not pts:
            continue
        depth = f["attrs"].get("DRVAL1", f["attrs"].get("VALDCO", float("nan")))
        flags = 1 if f["geom"] == "area" else 0
        body += struct.pack("<BBfH", L, flags, depth, len(pts))
        for la, lo in pts:
            body += struct.pack("<ff", la, lo)
        n += 1
    hdr = struct.pack("<IBBHffffI", MAGIC, 1, NLAYERS, 0,
                      cell["bbox"][0], cell["bbox"][1], cell["bbox"][2], cell["bbox"][3], n)
    return hdr + body, n


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(__file__), "tiles")
    tol = float(sys.argv[2]) if len(sys.argv) > 2 else 3e-4
    os.makedirs(outdir, exist_ok=True)

    inter = pickle.load(open(os.path.join(os.path.dirname(__file__), "intermediate.pkl"), "rb"))
    total = 0
    for cell in inter:
        blob, n = pack_cell(cell, tol)
        fn = os.path.join(outdir, f"{cell['cellid']:08d}.c93t")
        open(fn, "wb").write(blob)
        total += len(blob)
        if n:
            print(f"  {os.path.basename(fn)}: {n:5d} feats  {len(blob)/1024:7.1f} KB")
    print(f"wrote {len(inter)} tiles -> {outdir}  ({total/1e6:.2f} MB total, tol={tol})")


if __name__ == "__main__":
    main()
