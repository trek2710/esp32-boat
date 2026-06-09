#!/usr/bin/env python3
"""Stage A — extract ALL CM93 features in the Copenhagen box to one
intermediate pickle. The slow/complex CM93 step, done once. Adding or
re-tuning layers later runs only stage B (cm93_pack.py) off this file —
never re-touches CM93.

Usage: python3 cm93_extract.py [chart_root] [scale]   (default scale C)
Out:   intermediate.pkl  — list of {cellid, bbox, features:[{cls,geom,points,attrs}]}
"""
import os, sys, time, pickle, cm93_decode as m

# Box ~500 km around Copenhagen: lat 51.5–60.5, lon 4–20.
BOX = (51.5, 4.0, 60.5, 20.0)


def enumerate_cells(chart, scale):
    la0, lo0, la1, lo1 = BOX
    seen = {}
    la = la0
    while la <= la1:
        lo = lo0
        while lo <= lo1:
            p = m.cell_path(chart, la, lo, scale)
            if p:
                seen[p] = int(os.path.basename(p).split(".")[0])
            lo += 1.0
        la += 1.0
    return seen


def main():
    chart = sys.argv[1] if len(sys.argv) > 1 else \
        "/Users/jeppekoefoed/Documents/Charts/cm93_World2014"
    scale = sys.argv[2] if len(sys.argv) > 2 else "C"

    dec = m.build_decode_table()
    objd, ad = m.load_obj_dict(chart), m.load_attr_dict(chart)
    cells = enumerate_cells(chart, scale)
    print(f"{len(cells)} {scale}-scale cells in box")

    out, t0, nf = [], time.time(), 0
    for path, cid in sorted(cells.items()):
        c = m.decode_cell(path, dec)
        if not c["integrity"]:
            print(f"  !! {path} failed integrity, skipped"); continue
        feats = m.build_features(c, objd, ad)
        nf += len(feats)
        out.append(dict(cellid=cid, bbox=c["bbox"], features=feats))

    outp = os.path.join(os.path.dirname(__file__), "intermediate.pkl")
    pickle.dump(out, open(outp, "wb"), protocol=4)
    print(f"wrote {outp}: {len(out)} cells, {nf} features, "
          f"{os.path.getsize(outp)/1e6:.1f} MB, {time.time()-t0:.1f}s")


if __name__ == "__main__":
    main()
