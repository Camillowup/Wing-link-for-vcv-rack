#!/usr/bin/env python3
"""Shared by the panel scripts (fonts, colours, the Panel class, Rack components in previews). From the panels of WING-LINK (the bridge to a Behringer WING over USB) and its expander WING-LINK X, in the look of the GENRA plugin.

Eight strips per module, one per WING channel: the strip's display (colour, name, icon, channel: drawn by the module),
PAN / FX 1 / FX 2, MUTE / SOLO, a fader with the meter beside it, then the jacks: AUDIO (to the WING over USB),
CV for FADER / PAN / FX 1 / FX 2, and the outputs FADER and MUTE on the accent plates.

Usage:  winglink_panel.py real                   -> res/WingLink.svg, res/WingLinkX.svg, res/wing/*.svg (text as paths)
                                                    + src/WingLinkLayout.hpp
        winglink_panel.py preview OUT.png [RACK] -> preview of both modules side by side with Rack components
                                                    (RACK: a Rack source tree, for res/ComponentLibrary)

Coordinates in mm (30 HP each, height 128.5 mm). The C++ side converts with mm2px().
"""
import os
import re
import subprocess
import sys

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

PREVIEW = False
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RACK = os.environ.get("RACK", os.path.join(ROOT, "..", "ext", "Rack"))
CL = os.path.join(RACK, "res", "ComponentLibrary")

HP = 33
W, H = HP * 5.08, 128.5
BG, PLATE, LINE, TXT, DIM, DARK = "#17181d", "#212329", "#34363f", "#d9dbe2", "#8a8d99", "#07080a"
TITLE, RING = "#e6e7ec", "#4a4c57"
ACC = "#ff6b5b"  # coral: WING-LINK plays no GENRA track, so none of the four track colours (amber is not wanted)
R = {"small": 3.84, "jack": 4.0, "button": 3.6}

FONT_DIR = "/usr/share/fonts/opentype/inter/"
FONTS = {600: "Inter-SemiBold.otf", 700: "Inter-Bold.otf", 800: "Inter-ExtraBold.otf"}
_fonts = {}


def font(weight):
    if weight not in _fonts:
        f = TTFont(FONT_DIR + FONTS[weight])
        _fonts[weight] = (f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm, f["hmtx"])
    return _fonts[weight]


def width(s, size, weight, sp):
    gs, cmap, upm, hmtx = font(weight)
    return sum(hmtx[cmap.get(ord(c), "space")][0] * size / upm + sp for c in s) - sp


def mixc(col, f):
    c = [int(col[i:i + 2], 16) for i in (1, 3, 5)]
    p = [int(PLATE[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(p[i] + (c[i] - p[i]) * f):02x}" for i in range(3))


OUT_TINT = mixc(ACC, 0.32)

# ---------------------------------------------------------------- geometry shared by both modules (and the C++ side)
LABEL_W = 11.0        # the row labels on the left
SP = 19.0             # strip pitch
X0 = LABEL_W + 1.5    # left edge of strip 1
SCRIB = (25.0, 36.0)  # the strip's display (top, bottom)
FADER_TOP, FADER_BOT = 40.0, 80.0
FADER_W, HANDLE_H = 5.6, 8.0
ROWS = {"PAN": 43.0, "FX 1": 52.0, "FX 2": 61.0, "MUTE": 70.0, "SOLO": 78.5}
JROWS = [87.6, 97.2, 106.8, 117.4]


def strip_x(i):
    x0 = X0 + i * SP
    cx = x0 + SP / 2
    return x0, cx


class Panel:
    def __init__(self, ns):
        self.ns = ns
        self.out, self.comps, self.areas = [], [], {}

    def e(self, s):
        self.out.append(s)

    def text(self, x, y, s, size=1.8, color=DIM, weight=600, anchor="middle", sp=0.25):
        gs, cmap, upm, hmtx = font(weight)
        scale = size / upm
        glyphs = [cmap.get(ord(c), "space") for c in s]
        w = sum(hmtx[g][0] * scale + sp for g in glyphs) - sp
        cx = x - (w / 2 if anchor == "middle" else (w if anchor == "end" else 0))
        paths = []
        for g in glyphs:
            pen = SVGPathPen(gs)
            gs[g].draw(TransformPen(pen, (scale, 0, 0, -scale, cx, y)))
            if pen.getCommands():
                paths.append(pen.getCommands())
            cx += hmtx[g][0] * scale + sp
        if paths:
            self.e(f'<path fill="{color}" d="{" ".join(paths)}"/>')

    def knob(self, name, x, y):
        self.comps.append(("small", name, x, y))
        r = R["small"] + 0.5
        self.e(f'<circle cx="{x:.2f}" cy="{y}" r="{r:.2f}" fill="none" stroke="{RING}" stroke-width="0.29" '
               f'stroke-dasharray="0.29 0.73" stroke-linecap="round"/>')

    def button(self, name, x, y):
        self.comps.append(("button", name, x, y))

    def jack(self, name, x, y, label, out_=False):
        self.comps.append(("output" if out_ else "input", name, x, y))
        if out_:
            self.e(f'<rect x="{x - 4.1:.2f}" y="{y - 4.1:.2f}" width="8.2" height="10.3" rx="0.9" fill="{OUT_TINT}"/>')
        self.text(x, y + 5.45, label, 1.3, TXT if out_ else DIM, 600, "middle", 0.15)

    def area(self, name, x, y, w, h):
        self.areas[name] = (x, y, w, h)

    def header(self, title, subtitle):
        self.e(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W:.2f}mm" height="{H}mm" viewBox="0 0 {W:.2f} {H}">')
        self.e(f'<rect width="{W:.2f}" height="{H}" fill="{BG}"/>')
        self.text(12.9, 7.6, title, 5.1, TITLE, 800, "start", 1.08)
        self.text(12.9 + width(title, 5.1, 800, 1.08) + 3.4, 7.45, subtitle, 1.76, DIM, 600, "start", 0.47)
        self.e(f'<rect x="{W - 24:.2f}" y="5.55" width="6.4" height="1.08" rx="0.54" fill="{ACC}"/>')

    def strips(self):
        # row labels
        for lb, y in ROWS.items():
            self.text(LABEL_W - 0.2, y + 0.6, lb, 1.45, DIM, 700, "end", 0.25)
        self.text(LABEL_W - 0.2, JROWS[0] + 0.6, "IN", 1.45, DIM, 700, "end", 0.25)
        self.text(LABEL_W - 0.2, JROWS[3] + 0.6, "OUT", 1.45, ACC, 800, "end", 0.25)
        for i in range(8):
            n = i + 1
            x0, cx = strip_x(i)
            self.e(f'<rect x="{x0 + 0.4:.2f}" y="23.5" width="{SP - 0.8:.2f}" height="100.6" rx="1.2" fill="{PLATE}"/>')
            # the strip's display (the module draws colour, name, icon, channel)
            self.e(f'<rect x="{x0 + 1.1:.2f}" y="{SCRIB[0]}" width="{SP - 2.2:.2f}" height="{SCRIB[1] - SCRIB[0]}" '
                   f'rx="0.8" fill="{DARK}"/>')
            self.area(f"SCRIB{n}", x0 + 1.1, SCRIB[0], SP - 2.2, SCRIB[1] - SCRIB[0])
            fx, kx = cx - 5.2, cx + 4.9
            # fader slot (the slider's own background draws the track)
            self.comps.append(("fader", f"FADER{n}", fx, (FADER_TOP + FADER_BOT) / 2))
            # meter beside the fader
            self.e(f'<rect x="{cx - 1.75:.2f}" y="{FADER_TOP + 1}" width="1.2" height="{FADER_BOT - FADER_TOP - 2}" '
                   f'rx="0.3" fill="#111215"/>')
            self.area(f"VU{n}", cx - 1.75, FADER_TOP + 1, 1.2, FADER_BOT - FADER_TOP - 2)
            self.knob(f"PAN{n}", kx, ROWS["PAN"])
            self.knob(f"FX1_{n}", kx, ROWS["FX 1"])
            self.knob(f"FX2_{n}", kx, ROWS["FX 2"])
            self.button(f"MUTE{n}", kx, ROWS["MUTE"])
            self.button(f"SOLO{n}", kx, ROWS["SOLO"])
            self.e(f'<line x1="{x0 + 2:.2f}" y1="83.0" x2="{x0 + SP - 2:.2f}" y2="83.0" stroke="{LINE}" stroke-width="0.3"/>')
            jl, jr = cx - 4.75, cx + 4.75
            self.jack(f"AUDIO{n}", cx, JROWS[0], "AUDIO")
            self.jack(f"CV_FDR{n}", jl, JROWS[1], "FADER")
            self.jack(f"CV_PAN{n}", jr, JROWS[1], "PAN")
            self.jack(f"CV_FX1_{n}", jl, JROWS[2], "FX 1")
            self.jack(f"CV_FX2_{n}", jr, JROWS[2], "FX 2")
            self.jack(f"OUT_FDR{n}", jl, JROWS[3], "FADER", True)
            self.jack(f"OUT_MUTE{n}", jr, JROWS[3], "MUTE", True)

    def finish(self):
        self.e("</svg>")



_cache = {}


def img(out, name, x, y, path=None):
    fn = path or os.path.join(CL, name + ".svg")
    if fn not in _cache:
        src = open(fn).read()
        src = re.sub(r"<\?xml[^>]*\?>|<!DOCTYPE[^>]*>|<!--.*?-->", "", src, flags=re.S)
        m = re.search(r"<svg[^>]*>", src, re.S)
        tag = m.group(0)
        wm = re.search(r'\swidth="([0-9.]+)(mm|px)?', tag)
        hm = re.search(r'\sheight="([0-9.]+)(mm|px)?', tag)
        k = 1.0 if wm.group(2) == "mm" else 1 / 2.9528
        vb = re.search(r'viewBox="([^"]+)"', tag)
        body = src[m.end():src.rindex("</svg>")]
        body = re.sub(r"<sodipodi:namedview.*?(/>|</sodipodi:namedview>)|<metadata.*?</metadata>", "", body, flags=re.S)
        body = re.sub(r'\s(inkscape|sodipodi):[a-zA-Z-]+="[^"]*"', "", body)
        _cache[fn] = (vb.group(1) if vb else f"0 0 {wm.group(1)} {hm.group(1)}", float(wm.group(1)) * k,
                      float(hm.group(1)) * k, body)
    vbs, sw, sh, body = _cache[fn]
    out.insert(-1, f'<svg x="{x - sw / 2:.3f}" y="{y - sh / 2:.3f}" width="{sw:.3f}" height="{sh:.3f}" '
                   f'viewBox="{vbs}" overflow="visible">{body}</svg>')


