#!/usr/bin/env python3
"""Render AIS-Radar app-icon candidates (1024², supersampled). Marine palette:
radar PPI + nautical chart + AIS triangle + threat ring."""
import os, math
from PIL import Image, ImageDraw

S = 4                      # supersample
N = 1024
W = N * S
OUT = os.path.dirname(__file__)

NAVY  = (10, 23, 40)
NAVY2 = (16, 38, 64)
CYAN  = (46, 200, 230)
SAND  = (230, 217, 168)
BLUE1 = (84, 145, 203)
BLUE2 = (169, 207, 236)
WHITE = (240, 246, 250)
GREEN = (18, 176, 33)
YELL  = (224, 196, 0)
RED   = (224, 32, 26)
MAG   = (224, 24, 106)


def new(bg=NAVY):
    im = Image.new("RGB", (W, W), bg)
    return im, ImageDraw.Draw(im)


def radial(d, cx, cy, r, c0, c1, steps=120):
    for i in range(steps, 0, -1):
        t = i / steps
        col = tuple(int(c1[k] + (c0[k] - c1[k]) * t) for k in range(3))
        rr = r * t
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=col)


def ring(d, cx, cy, r, w, col):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=col, width=w)


def tri(d, x, y, hdg, ln, col):
    t = math.radians(hdg); cs, sn = math.cos(t), math.sin(t)
    def m(lx, ly): return (x + lx * cs + ly * sn, y + lx * sn - ly * cs)
    d.polygon([m(0, ln), m(-ln * 0.62, -ln * 0.7), m(ln * 0.62, -ln * 0.7)], fill=col)


def save(im, name):
    im.resize((N, N), Image.LANCZOS).save(os.path.join(OUT, name))
    print("wrote", name)


# 1 — Radar sweep: navy PPI, sweep wedge, colored blips
def icon1():
    im, d = new(); cx = cy = W / 2
    radial(d, cx, cy, W * 0.72, NAVY2, NAVY)
    for k in (1, 2, 3):
        ring(d, cx, cy, W * 0.12 * k, S * 2, (40, 78, 110))
    d.line([cx, cy - W * 0.36, cx, cy + W * 0.36], fill=(40, 78, 110), width=S * 2)
    d.line([cx - W * 0.36, cy, cx + W * 0.36, cy], fill=(40, 78, 110), width=S * 2)
    # sweep wedge
    sweep = Image.new("RGBA", (W, W), (0, 0, 0, 0)); sd = ImageDraw.Draw(sweep)
    for i in range(60):
        a = -60 + i
        al = int(120 * (i / 60))
        sd.pieslice([cx - W * 0.36, cy - W * 0.36, cx + W * 0.36, cy + W * 0.36],
                    a, a + 1, fill=(46, 200, 230, al))
    im.paste(Image.alpha_composite(im.convert("RGBA"), sweep).convert("RGB"), (0, 0))
    d = ImageDraw.Draw(im)
    tri(d, cx + W * 0.16, cy - W * 0.13, 200, W * 0.05, GREEN)
    tri(d, cx - W * 0.18, cy + W * 0.10, 60, W * 0.05, YELL)
    tri(d, cx + W * 0.08, cy + W * 0.20, 320, W * 0.055, RED)
    tri(d, cx, cy, 0, W * 0.07, CYAN)
    save(im, "icon1_radar_sweep.png")


# 2 — Chartplotter: nautical chart disc + threat ring + own ship
def icon2():
    im, d = new(NAVY); cx = cy = W / 2; R = W * 0.46
    # water base
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=BLUE2)
    # depth band + land as simple lobes (clipped to circle via mask)
    chart = Image.new("RGB", (W, W), BLUE2); cd = ImageDraw.Draw(chart)
    cd.ellipse([cx - R, cy - R*1.6, cx + R*1.7, cy + R*0.2], fill=BLUE1)      # deeper band
    cd.polygon([(cx - R, cy - R), (cx + R*0.2, cy - R*0.7), (cx + R*0.1, cy - R*0.1),
                (cx - R*0.6, cy + R*0.1), (cx - R, cy + R*0.4)], fill=SAND)   # land
    mask = Image.new("L", (W, W), 0); ImageDraw.Draw(mask).ellipse(
        [cx - R, cy - R, cx + R, cy + R], fill=255)
    im.paste(chart, (0, 0), mask)
    d = ImageDraw.Draw(im)
    for k in (1, 2):
        ring(d, cx, cy, R * 0.42 * k, S * 2, (255, 255, 255, ))
    ring(d, cx, cy, R, S * 14, RED)                 # threat ring
    tri(d, cx + R*0.5, cy - R*0.45, 210, W*0.05, (20, 30, 40))
    tri(d, cx, cy, 0, W * 0.075, MAG)               # own ship
    save(im, "icon2_chartplotter.png")


# 3 — Threat ring: bold tri-colour rim + central vessel + course vector
def icon3():
    im, d = new(NAVY); cx = cy = W / 2; R = W * 0.44
    radial(d, cx, cy, R, NAVY2, NAVY)
    arcs = [(-90, 90, GREEN), (90, 210, YELL), (210, 270, RED)]
    for a0, a1, c in arcs:
        d.arc([cx - R, cy - R, cx + R, cy + R], a0, a1, fill=c, width=S * 26)
    for k in (1, 2):
        ring(d, cx, cy, R * 0.4 * k, S * 2, (52, 92, 120))
    d.line([cx, cy, cx + W * 0.0, cy - W * 0.26], fill=CYAN, width=S * 5)   # course vector
    tri(d, cx, cy, 0, W * 0.085, CYAN)
    save(im, "icon3_threat_ring.png")


# 4 — AIS chevron: one bold triangle over faint rings
def icon4():
    im, d = new(NAVY); cx = cy = W / 2
    radial(d, cx, cy, W * 0.7, NAVY2, NAVY)
    for k in (1, 2, 3):
        ring(d, cx, cy, W * 0.13 * k, S * 2, (38, 74, 104))
    # glow triangle
    glow = Image.new("RGBA", (W, W), (0, 0, 0, 0)); gd = ImageDraw.Draw(glow)
    tri(gd, cx, cy + W * 0.05, 0, W * 0.34, (46, 200, 230, 60))
    im.paste(Image.alpha_composite(im.convert("RGBA"), glow).convert("RGB"), (0, 0))
    d = ImageDraw.Draw(im)
    tri(d, cx, cy + W * 0.05, 0, W * 0.27, CYAN)
    tri(d, cx, cy + W * 0.05, 0, W * 0.135, NAVY)        # inner cut for a chevron feel
    save(im, "icon4_ais_chevron.png")


# 5 — Minimal PPI: clean cyan rings on navy + N tick
def icon5():
    im, d = new(NAVY); cx = cy = W / 2
    radial(d, cx, cy, W * 0.72, NAVY2, NAVY)
    for k in (1, 2, 3):
        ring(d, cx, cy, W * 0.12 * k, S * 3, CYAN if k == 3 else (40, 110, 140))
    d.line([cx, cy - W * 0.36, cx, cy - W * 0.30], fill=CYAN, width=S * 5)
    tri(d, cx, cy, 0, W * 0.085, CYAN)
    tri(d, cx + W*0.17, cy + W*0.05, 250, W*0.05, RED)
    save(im, "icon5_minimal_ppi.png")


# 6 — Day chart: light/paper nautical look, magenta own ship, red target
def icon6():
    im, d = new((233, 241, 247)); cx = cy = W / 2
    d.rectangle([0, 0, W, W*0.5], fill=(201, 224, 240))
    d.polygon([(0, 0), (W*0.55, 0), (W*0.42, W*0.28), (W*0.1, W*0.34), (0, W*0.5)],
              fill=SAND)
    d.line([(W*0.55, 0), (W*0.42, W*0.28), (W*0.1, W*0.34), (0, W*0.5)],
           fill=(53, 67, 79), width=S*4)
    for k in (1, 2, 3):
        ring(d, cx, cy, W * 0.12 * k, S * 2, (110, 122, 134))
    ring(d, cx, cy, W * 0.47, S * 18, GREEN)
    tri(d, cx + W*0.16, cy - W*0.14, 210, W*0.05, RED)
    tri(d, cx, cy, 30, W * 0.08, MAG)
    save(im, "icon6_day_chart.png")


for f in (icon1, icon2, icon3, icon4, icon5, icon6):
    try: f()
    except Exception as e: print(f.__name__, "FAILED", e)
