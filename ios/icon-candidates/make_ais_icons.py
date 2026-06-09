#!/usr/bin/env python3
"""Radar-sweep app icon with 'AIS' wordmark — a few treatments to choose from."""
import os, math
from PIL import Image, ImageDraw, ImageFont

S = 4; N = 1024; W = N * S
OUT = os.path.dirname(__file__)
NAVY=(10,23,40); NAVY2=(16,38,64); CYAN=(46,200,230); GREEN=(18,176,33)
YELL=(224,196,0); RED=(224,32,26); WHITE=(240,246,250); GRID=(40,78,110)

FONT = {
    "din_cond": "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
    "din_alt":  "/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf",
    "menlo":    "/System/Library/Fonts/Menlo.ttc",
    "impact":   "/System/Library/Fonts/Supplemental/Impact.ttf",
}
def font(key, px):
    return ImageFont.truetype(FONT[key], px * S)

def tri(d, x, y, hdg, ln, col):
    t=math.radians(hdg); cs,sn=math.cos(t),math.sin(t)
    m=lambda lx,ly:(x+lx*cs+ly*sn, y+lx*sn-ly*cs)
    d.polygon([m(0,ln),m(-ln*0.62,-ln*0.7),m(ln*0.62,-ln*0.7)],fill=col)

def radial(d,cx,cy,r,c0,c1,steps=120):
    for i in range(steps,0,-1):
        t=i/steps; col=tuple(int(c1[k]+(c0[k]-c1[k])*t) for k in range(3)); rr=r*t
        d.ellipse([cx-rr,cy-rr,cx+rr,cy+rr],fill=col)

def radar_base(cy_frac=0.46, ring=0.30):
    """Radar sweep, centre lifted a touch to leave room for the wordmark."""
    im=Image.new("RGB",(W,W),NAVY); d=ImageDraw.Draw(im)
    cx=W/2; cy=W*cy_frac
    radial(d,cx,cy,W*0.62,NAVY2,NAVY)
    for k in (1,2,3):
        d.ellipse([cx-W*ring/3*k,cy-W*ring/3*k,cx+W*ring/3*k,cy+W*ring/3*k],outline=GRID,width=S*2)
    d.line([cx,cy-W*ring,cx,cy+W*ring],fill=GRID,width=S*2)
    d.line([cx-W*ring,cy,cx+W*ring,cy],fill=GRID,width=S*2)
    sweep=Image.new("RGBA",(W,W),(0,0,0,0)); sd=ImageDraw.Draw(sweep)
    for i in range(60):
        a=-70+i; sd.pieslice([cx-W*ring,cy-W*ring,cx+W*ring,cy+W*ring],a,a+1,fill=(46,200,230,int(120*i/60)))
    im=Image.alpha_composite(im.convert("RGBA"),sweep).convert("RGB"); d=ImageDraw.Draw(im)
    tri(d,cx+W*0.15,cy-W*0.11,205,W*0.045,GREEN)
    tri(d,cx-W*0.16,cy-W*0.02,60,W*0.045,YELL)
    tri(d,cx,cy,0,W*0.06,CYAN)
    return im,cx,cy

def wordmark(d, text, key, px, x, y, col, track=0):
    f=font(key,px)
    if track==0:
        d.text((x,y),text,font=f,fill=col,anchor="mm")
    else:
        # manual letter spacing
        widths=[d.textbbox((0,0),c,font=f)[2] for c in text]
        total=sum(widths)+track*S*(len(text)-1)
        cx0=x-total/2
        for c,w in zip(text,widths):
            d.text((cx0,y),c,font=f,fill=col,anchor="lm"); cx0+=w+track*S

def save(im,name): im.resize((N,N),Image.LANCZOS).save(os.path.join(OUT,name)); print("wrote",name)

# A — DIN Condensed, cyan, bottom
im,cx,cy=radar_base(); d=ImageDraw.Draw(im)
wordmark(d,"AIS","din_cond",260,cx,W*0.84,CYAN,track=10); save(im,"ais_A_dincond_cyan.png")

# B — DIN Alternate, white, bottom
im,cx,cy=radar_base(); d=ImageDraw.Draw(im)
wordmark(d,"AIS","din_alt",210,cx,W*0.85,WHITE,track=6); save(im,"ais_B_dinalt_white.png")

# C — Menlo mono, cyan, spaced
im,cx,cy=radar_base(); d=ImageDraw.Draw(im)
wordmark(d,"AIS","menlo",150,cx,W*0.85,CYAN,track=30); save(im,"ais_C_menlo.png")

# D — big watermark behind, DIN Condensed, faint cyan, centred
im=Image.new("RGB",(W,W),NAVY); d=ImageDraw.Draw(im)
radial(d,W/2,W/2,W*0.62,NAVY2,NAVY)
wm=Image.new("RGBA",(W,W),(0,0,0,0)); wd=ImageDraw.Draw(wm)
wordmark(wd,"AIS","din_cond",520,W/2,W*0.5,(46,200,230,70),track=20)
im=Image.alpha_composite(im.convert("RGBA"),wm).convert("RGB"); d=ImageDraw.Draw(im)
for k in (1,2,3): d.ellipse([W/2-W*0.10*k,W/2-W*0.10*k,W/2+W*0.10*k,W/2+W*0.10*k],outline=GRID,width=S*2)
tri(d,W/2+W*0.14,W/2-W*0.10,205,W*0.045,GREEN); tri(d,W/2,W/2,0,W*0.06,CYAN)
save(im,"ais_D_watermark.png")

# E — DIN Condensed, cyan, with a thin underline rule
im,cx,cy=radar_base(); d=ImageDraw.Draw(im)
wordmark(d,"AIS","din_cond",250,cx,W*0.82,WHITE,track=12)
d.line([cx-W*0.16,W*0.90,cx+W*0.16,W*0.90],fill=CYAN,width=S*5); save(im,"ais_E_underline.png")
