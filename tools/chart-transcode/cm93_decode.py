#!/usr/bin/env python3
"""CM93 v2 cell decoder — SPIKE.

Goal: prove we can pull real geometry (coastline first) out of a real CM93
cell, so we can decide whether to build the offline transcoder on CM93 or
fall back to open data. Ported from OpenCPN's cm93.cpp (GPL) — see
reference/cm93.cpp. This is a reading/validation spike, not production.

Format recap (all multi-byte little-endian, every byte XOR-substituted
through Decode_table built from Table_0):
  prolog  : u16 lenProlog+header(=138), i32 lenTable1, i32 lenTable2
            (the three sum to the file length — integrity check)
  header  : 8 doubles bbox (lon/lat min/max, then Mercator E/N min/max)
            + record counts (vectors, point3d, point2d, features)
  table1  : vector(edge) records, point3d records, point2d records
  table2  : feature(object) records
Geometry points are u16 (x,y) mapped linearly across the cell, then inverse
spherical-Mercator on the International-1924 axis -> lat/lon.
"""

import os, sys, glob, math, struct, json

A = 6378388.0                  # CM93 semimajor axis (International 1924)
DEG = math.pi / 180.0

# Per-byte de-obfuscation table, == inverse of (Table_0[i] ^ 8) from
# OpenCPN cm93.cpp. Precomputed so this script stands alone.
DECODE_TABLE = bytes((
    97,92,63,174,76,74,47,30,36,158,28,214,128,13,115,126,
    55,239,65,203,237,147,87,240,246,173,184,188,64,221,204,149,
    151,71,252,171,170,153,248,136,228,179,62,132,33,152,131,32,
    191,18,155,53,94,215,4,80,59,180,223,127,35,236,212,129,
    3,49,234,141,108,186,29,84,138,34,253,189,218,190,168,198,
    134,54,37,116,251,27,139,238,172,245,8,122,202,123,21,169,
    24,90,200,208,43,5,25,145,17,249,142,227,113,124,105,104,
    114,206,22,7,20,241,205,140,242,211,216,146,44,73,166,225,
    229,199,187,219,103,125,11,58,143,89,196,226,86,19,57,193,
    156,119,100,117,83,250,46,222,162,209,130,167,72,165,45,15,
    61,56,70,12,75,231,106,95,194,31,210,96,10,109,185,14,
    93,16,23,78,161,207,254,98,163,88,66,137,91,133,112,213,
    201,67,6,39,150,0,120,220,52,175,48,60,224,79,217,68,
    183,82,135,121,2,160,181,148,40,244,192,247,107,85,178,101,
    233,235,1,159,110,69,154,195,99,9,157,50,144,51,255,177,
    42,111,41,243,164,81,38,182,176,197,118,232,77,26,230,102,
))
assert len(DECODE_TABLE) == 256


def build_decode_table():
    return DECODE_TABLE


def load_obj_dict(chart_root):
    """CM93OBJ.DIC: 'CLASSNAME|classnum|geomtype' -> {classnum: name}."""
    for cand in ("CM93OBJ.DIC", "cm93obj.dic"):
        p = os.path.join(chart_root, cand)
        if os.path.exists(p):
            d = {}
            for line in open(p, encoding="latin-1"):
                parts = line.strip().split("|")
                if len(parts) >= 2 and parts[1].strip().isdigit():
                    d[int(parts[1])] = parts[0].strip()
            return d
    return {}


# ---- CM93 cell-index / file-path math (Get_CM93_CellIndex + Is_CM93Cell_Present)
DVAL = {"Z": 120, "A": 60, "B": 30, "C": 12, "D": 3, "E": 1, "F": 1, "G": 1}


def cell_index(lat, lon, scale):
    dval = DVAL[scale]
    lon1 = (lon + 360.0) * 3.0
    while lon1 >= 1080.0:
        lon1 -= 1080.0
    lon3 = int(lon1 // dval) * dval
    lat1 = lat * 3.0 + 270.0 - 30.0
    lat3 = int(lat1 // dval) * dval
    return (lat3 + 30) * 10000 + lon3


def cell_path(chart_root, lat, lon, scale):
    """Resolve the on-disk cell file for a lat/lon at a scale letter."""
    dval = DVAL[scale]
    ci = cell_index(lat, lon, scale)
    ilat, ilon = ci // 10000, ci % 10000
    jlat = (((ilat - 30) // dval) * dval) + 30
    jlon = (ilon // dval) * dval
    ilatroot = (((ilat - 30) // 60) * 60) + 30
    ilonroot = (ilon // 60) * 60
    root = f"{ilatroot:04d}{ilonroot:04d}"
    stem = f"{jlat:03d}{jlon:04d}"          # 7 digits; real name has a 1-char prefix
    for sc in (scale, scale.lower()):
        d = os.path.join(chart_root, root, sc)
        hits = glob.glob(os.path.join(d, f"?{stem}.{sc}"))
        if hits:
            return hits[0]
    return None


class Cur:
    """Little-endian cursor over the already-de-obfuscated cell buffer."""
    def __init__(self, buf, off=0):
        self.b, self.o = buf, off
    def u16(self):
        v = struct.unpack_from("<H", self.b, self.o)[0]; self.o += 2; return v
    def i32(self):
        v = struct.unpack_from("<i", self.b, self.o)[0]; self.o += 4; return v
    def u8(self):
        v = self.b[self.o]; self.o += 1; return v
    def skip(self, n):
        self.o += n


HDR = "<HiiiHiiHHHHiiHHHiiHii"   # 21 count fields after the 8 bbox doubles (64B)


def decode_cell(path, decode_table):
    raw = open(path, "rb").read()
    buf = bytes(decode_table[c] for c in raw)        # whole-file substitution

    c = Cur(buf)
    word0, t1len, t2len = c.u16(), c.i32(), c.i32()  # 10-byte prolog
    integrity = (word0 + t1len + t2len) == len(raw)

    lon_min, lat_min, lon_max, lat_max, e_min, n_min, e_max, n_max = \
        struct.unpack_from("<8d", buf, c.o); c.o += 64
    h = struct.unpack_from(HDR, buf, c.o); c.o += struct.calcsize(HDR)
    (n_vec, n_vec_pts, m_46, m_4a, n_p3d, m_50, m_54, n_p2d, m_5a, m_5c,
     n_feat, m_60, m_64, m_68, m_6a, m_6c, n_relptr, m_72, m_76, m_78, m_7c) = h

    # transform coefficients
    dx = e_max - e_min
    if dx < 0:
        dx += A * 2.0 * math.pi
    xr, yr = dx / 65535.0, (n_max - n_min) / 65535.0
    xo, yo = e_min, n_min

    def to_ll(x, y):
        vx, vy = x * xr + xo, y * yr + yo
        lat = (2.0 * math.atan(math.exp(vy / A)) - math.pi / 2.0) / DEG
        lon = vx / (DEG * A)
        return lat, lon

    # ---- table 1: edges, point3d, point2d (body starts right after header)
    edges = []
    for _ in range(n_vec):
        npts = c.u16()
        pts = [(c.u16(), c.u16()) for _ in range(npts)]
        edges.append(pts)
    for _ in range(n_p3d):                # soundings: skip geometry for the spike
        npts = c.u16()
        c.skip(npts * 6)
    p2d = [(c.u16(), c.u16()) for _ in range(n_p2d)]

    # ---- table 2: feature records
    feats = []
    for _ in range(n_feat):
        otype = c.u8(); geo = c.u8(); desc = c.u16()
        prim = geo & 0x0f
        geom = None
        if prim in (4, 2):                       # AREA / LINE: list of edge refs
            ne = c.u16(); desc -= ne * 2 + 2
            geom = [c.u16() for _ in range(ne)]
        elif prim == 1:                          # 2D point
            geom = ("p2d", c.u16()); desc -= 2
        elif prim == 8:                          # 3D point
            geom = ("p3d", c.u16()); desc -= 2
        if geo & 0x10:                           # related objects (1-byte count)
            nr = c.u8(); desc -= nr * 2 + 1
            c.skip(nr * 2)
        if geo & 0x20:                           # related (2-byte count)
            c.u16(); desc -= 2
        attrs = None
        if geo & 0x80:                           # attribute block
            nattr = c.u8(); desc -= 5
            attrs = (nattr, c.o, desc)           # (count, offset, byte-len) — decode later
            c.skip(desc)
        feats.append((otype, prim, geom, attrs))

    return dict(path=path, integrity=integrity, word0=word0,
                bbox=(lat_min, lon_min, lat_max, lon_max),
                counts=dict(vec=n_vec, p3d=n_p3d, p2d=n_p2d, feat=n_feat),
                edges=edges, p2d=p2d, feats=feats, to_ll=to_ll, end=c.o, len=len(raw))


def build_lines(cell, class_filter):
    """Resolve features whose class is in class_filter into lat/lon polylines."""
    out = []
    for otype, prim, geom, attrs in cell["feats"]:
        if otype not in class_filter or prim not in (2, 4) or not geom:
            continue
        line = []
        for ref in geom:
            idx = ref & 0x1fff
            if idx >= len(cell["edges"]):
                continue
            for (x, y) in cell["edges"][idx]:
                line.append(cell["to_ll"](x, y))
        if len(line) >= 2:
            out.append(line)
    return out


def main():
    chart = sys.argv[1] if len(sys.argv) > 1 else \
        "/Users/jeppekoefoed/Documents/Charts/cm93_World2014"
    lat = float(sys.argv[2]) if len(sys.argv) > 2 else 55.68   # Copenhagen
    lon = float(sys.argv[3]) if len(sys.argv) > 3 else 12.57
    scale = sys.argv[4] if len(sys.argv) > 4 else "C"

    dec = build_decode_table()
    objd = load_obj_dict(chart)
    name2num = {v: k for k, v in objd.items()}
    print(f"dict: {len(objd)} classes; "
          f"COALNE={name2num.get('COALNE')} DEPARE={name2num.get('DEPARE')} "
          f"TSSLPT={name2num.get('TSSLPT')} BOYLAT={name2num.get('BOYLAT')} "
          f"LIGHTS={name2num.get('LIGHTS')}")

    path = cell_path(chart, lat, lon, scale)
    print(f"cell for ({lat},{lon}) scale {scale}: {path}")
    if not path:
        print("no cell found at that scale; try B or D"); return

    cell = decode_cell(path, dec)
    print(f"integrity={cell['integrity']} prolog={cell['word0']} "
          f"parsed_to={cell['end']}/{cell['len']} bbox={cell['bbox']}")
    print(f"counts={cell['counts']}")

    # class histogram
    hist = {}
    for otype, *_ in cell["feats"]:
        hist[otype] = hist.get(otype, 0) + 1
    top = sorted(hist.items(), key=lambda kv: -kv[1])[:15]
    print("top classes: " + ", ".join(
        f"{objd.get(k, '?'+str(k))}:{v}" for k, v in top))

    coalne = name2num.get("COALNE")
    lines = build_lines(cell, {coalne}) if coalne is not None else []
    npts = sum(len(l) for l in lines)
    print(f"COALNE: {len(lines)} polylines, {npts} points")
    if lines:
        lats = [p[0] for l in lines for p in l]
        lons = [p[1] for l in lines for p in l]
        print(f"coastline lat {min(lats):.4f}..{max(lats):.4f}  "
              f"lon {min(lons):.4f}..{max(lons):.4f}")
        print(f"sample pts: {[ (round(a,4),round(b,4)) for a,b in lines[0][:4] ]}")

        gj = {"type": "FeatureCollection", "features": [
            {"type": "Feature", "properties": {"class": "COALNE"},
             "geometry": {"type": "LineString",
                          "coordinates": [[round(lo, 6), round(la, 6)] for la, lo in l]}}
            for l in lines]}
        outp = os.path.join(os.path.dirname(__file__), "spike_out.geojson")
        json.dump(gj, open(outp, "w"))
        print(f"wrote {outp}  (drop on geojson.io to eyeball the shape)")


if __name__ == "__main__":
    main()
