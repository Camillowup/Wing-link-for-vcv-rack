#!/usr/bin/env python3
"""Panels of WING-LINK (the bridge to a Behringer WING over USB) and its expander WING-LINK X, in the look of the GENRA plugin.

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

MODE = sys.argv[1] if len(sys.argv) > 1 else "real"
PREVIEW = MODE == "preview"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RACK = sys.argv[3] if PREVIEW and len(sys.argv) > 3 else os.path.join(ROOT, "..", "ext", "Rack")
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
ROWS = {"PAN": 42.0, "FX 1": 52.0, "FX 2": 62.0, "MUTE": 71.2, "SOLO": 79.2}
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
            self.text(LABEL_W - 0.2, y + 0.75, lb, 1.75, TXT, 800, "end", 0.3)
        for k in (1, 2):  # the bus each send goes to (the module writes it)
            self.area(f"BUSLBL{k}", 0.6, ROWS[f"FX {k}"] + 1.5, LABEL_W - 0.8, 2.6)
        self.text(LABEL_W - 0.2, JROWS[0] + 0.75, "IN", 1.75, TXT, 800, "end", 0.3)
        self.text(LABEL_W - 0.2, JROWS[3] + 0.75, "OUT", 1.75, ACC, 800, "end", 0.3)
        for i in range(8):
            n = i + 1
            x0, cx = strip_x(i)
            self.e(f'<rect x="{x0 + 0.4:.2f}" y="23.5" width="{SP - 0.8:.2f}" height="100.6" rx="1.2" fill="{PLATE}"/>')
            # the strip's display (the module draws colour, name, icon, channel)
            self.e(f'<rect x="{x0 + 1.1:.2f}" y="{SCRIB[0]}" width="{SP - 2.2:.2f}" height="{SCRIB[1] - SCRIB[0]}" '
                   f'rx="0.8" fill="{DARK}"/>')
            self.area(f"SCRIB{n}", x0 + 1.1, SCRIB[0], SP - 2.2, SCRIB[1] - SCRIB[0])
            fx, kx = cx - 5.2, cx + 4.55
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


# ---------------------------------------------------------------- WING-LINK
main = Panel("wl")
main.header("WING-LINK", "BEHRINGER WING BRIDGE · USB")
DISP = (12.5, 10.6, 66.0, 11.2)
main.e(f'<rect x="{DISP[0]}" y="{DISP[1]}" width="{DISP[2]}" height="{DISP[3]}" rx="1" fill="{DARK}" stroke="{LINE}" '
       f'stroke-width="0.3"/>')
main.area("DISPLAY", *DISP)
bx = DISP[0] + DISP[2]
main.button("DOWN", bx + 6.0, 15.4)
main.button("UP", bx + 23.5, 15.4)
main.text(bx + 14.75, 9.2, "START CH", 1.4, DIM, 700, "middle", 0.25)
main.e(f'<rect x="{bx + 11.6:.2f}" y="12.2" width="6.3" height="6.4" rx="0.8" fill="{DARK}" stroke="{LINE}" stroke-width="0.3"/>')
main.area("STEP", bx + 11.6, 12.2, 6.3, 6.4)  # the step (1 or 8): the module writes it; a click switches
main.text(bx + 14.75, 21.6, "STEP", 1.3, DIM, 700, "middle", 0.2)
main.button("SYNC", bx + 33.2, 15.4)
main.text(bx + 33.2, 21.6, "SYNC", 1.4, ACC, 800, "middle", 0.3)
main.text(bx + 6.0, 21.6, "−", 1.9, TXT, 800)
main.text(bx + 23.5, 21.6, "+", 1.9, TXT, 800)
# the SD recorder (WING-LIVE card): two cards (the module draws them; a click starts / stops), the number of tracks
SDX = bx + 39.8
for k in (0, 1):
    main.area(f"SD{k + 1}", SDX + k * 10.2, 10.0, 8.4, 10.6)
    main.text(SDX + k * 10.2 + 4.2, 22.4, f"SD {k + 1}", 1.35, DIM, 700, "middle", 0.25)
TX = SDX + 25.0
main.text(TX + 2.6, 9.2, "TRACKS", 1.4, DIM, 700, "middle", 0.25)
for k, lb in enumerate(("8", "16")):
    main.e(f'<circle cx="{TX + k * 5.2:.2f}" cy="14.6" r="1.35" fill="#0d0e11"/>')
    main.text(TX + k * 5.2, 19.0, lb, 1.5, TXT, 700, "middle", 0.2)
main.areas["TRACKS"] = (TX, 14.6, 5.2, 3)  # (first lamp's centre, pitch: a lamp row)
main.strips()
main.finish()

# ---------------------------------------------------------------- WING-LINK X (expander, docks right of WING-LINK)
xp = Panel("wlx")
xp.header("WING-LINK X", "EXPANDER · 8 MORE WING CHANNELS")
xp.e(f'<rect x="{DISP[0]}" y="{DISP[1]}" width="{DISP[2]}" height="{DISP[3]}" rx="1" fill="{DARK}" stroke="{LINE}" '
     f'stroke-width="0.3"/>')
xp.area("DISPLAY", *DISP)
# its own START CH (follows WING-LINK until you page here)
xp.button("DOWN", bx + 6.0, 15.4)
xp.button("UP", bx + 23.5, 15.4)
xp.text(bx + 14.75, 9.2, "START CH", 1.4, DIM, 700, "middle", 0.25)
xp.e(f'<rect x="{bx + 11.6:.2f}" y="12.2" width="6.3" height="6.4" rx="0.8" fill="{DARK}" stroke="{LINE}" stroke-width="0.3"/>')
xp.area("STEP", bx + 11.6, 12.2, 6.3, 6.4)
xp.text(bx + 14.75, 21.6, "STEP", 1.3, DIM, 700, "middle", 0.2)
xp.text(bx + 6.0, 21.6, "−", 1.9, TXT, 800)
xp.text(bx + 23.5, 21.6, "+", 1.9, TXT, 800)
xp.strips()
xp.finish()


# ---------------------------------------------------------------- the fader (background + handle), in the look of the GENRA plugin
def fader_svgs():
    h = FADER_BOT - FADER_TOP
    bg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{FADER_W}mm" height="{h}mm" viewBox="0 0 {FADER_W} {h}">',
          f'<rect x="{FADER_W / 2 - 0.55}" y="{HANDLE_H / 2 - 0.6}" width="1.1" height="{h - HANDLE_H + 1.2}" rx="0.55" '
          f'fill="{DARK}"/>']
    travel = h - HANDLE_H
    for k in range(11):
        y = HANDLE_H / 2 + travel * k / 10
        wd = 1.3 if k in (0, 10, 2) else 0.8  # the long tick at k == 2: 0 dB (position 0.75)
        bg.append(f'<rect x="{FADER_W / 2 - 1.1 - wd:.2f}" y="{y - 0.12:.2f}" width="{wd}" height="0.24" fill="{RING}"/>')
    bg.append("</svg>")
    hd = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{FADER_W}mm" height="{HANDLE_H}mm" '
          f'viewBox="0 0 {FADER_W} {HANDLE_H}">',
          f'<rect x="0.15" y="0.15" width="{FADER_W - 0.3}" height="{HANDLE_H - 0.3}" rx="0.9" fill="#101114" '
          f'stroke="#2c2e35" stroke-width="0.3"/>',
          f'<rect x="0.9" y="{HANDLE_H / 2 - 0.35}" width="{FADER_W - 1.8}" height="0.7" rx="0.35" fill="#f2f3f7"/>',
          "</svg>"]
    return "\n".join(bg), "\n".join(hd)


FBG, FHD = fader_svgs()

if not PREVIEW:
    for p, fn in ((main, "WingLink.svg"), (xp, "WingLinkX.svg")):
        open(os.path.join(ROOT, "res", fn), "w").write("\n".join(p.out))
    os.makedirs(os.path.join(ROOT, "res", "wing"), exist_ok=True)
    open(os.path.join(ROOT, "res", "wing", "fader_bg.svg"), "w").write(FBG)
    open(os.path.join(ROOT, "res", "wing", "fader_handle.svg"), "w").write(FHD)
    lines = ["// Generated by tools/winglink_panel.py - do not edit", "#pragma once", "namespace wllayout {"]
    for p in (main, xp):
        lines.append(f"namespace {p.ns} {{")
        for kind, n, x, y in p.comps:
            lines.append(f"static const float {n}[2] = {{{x:.3f}f, {y:.3f}f}};")
        for n, (x, y, w, h) in p.areas.items():
            lines.append(f"static const float {n}[4] = {{{x:.3f}f, {y:.3f}f, {w:.3f}f, {h:.3f}f}};")
        lines.append("}")
    lines += [f"static const int HP = {HP};", f"static const float FADER_W = {FADER_W}f, FADER_H = {FADER_BOT - FADER_TOP}f, "
              f"HANDLE_H = {HANDLE_H}f;", "}"]
    os.makedirs(os.path.join(ROOT, "src"), exist_ok=True)
    open(os.path.join(ROOT, "src", "WingLinkLayout.hpp"), "w").write("\n".join(lines) + "\n")
    print("wrote res/WingLink.svg, res/WingLinkX.svg, res/wing/*.svg and src/WingLinkLayout.hpp")
    sys.exit(0)

# ---------------------------------------------------------------- preview: both modules, Rack components, mock displays
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


tmp = os.path.join("/tmp", "wing_fader")
os.makedirs(tmp, exist_ok=True)
open(os.path.join(tmp, "bg.svg"), "w").write(FBG)
open(os.path.join(tmp, "hd.svg"), "w").write(FHD)
KNOB = os.path.join(ROOT, "res", "knob")
HAVE_CL = os.path.isdir(CL)
NAMES = ["KICK", "SNARE", "HAT CL", "HAT OP", "CLAP", "TOM", "RIDE", "PERC"], ["BASS", "LEAD", "PADS", "ARP", "VOX",
                                                                               "GTR", "FX", "DRONE"]
COLS = ["#7b6cff", "#7b6cff", "#7b6cff", "#b8f23a", "#3dc9ff", "#e0779e", "#3dc9ff", "#c9ccd4"]
POS = [0.75, 0.7, 0.55, 0.8, 0.6, 0.5, 0.65, 0.4]
ICONS = ["DRM 0", "DRM 4", "DRM 9", "STR 2", "KEY 3", "KEY 6", "KEY 1", "SPC 2"]
for k, p in enumerate((main, xp)):
    o = p.out
    for kind, n, x, y in p.comps:
        if kind == "small":
            if HAVE_CL:
                img(o, "", x, y, os.path.join(KNOB, "small_bg.svg"))
            img(o, "", x, y, os.path.join(KNOB, "small.svg")) if os.path.exists(os.path.join(KNOB, "small.svg")) else None
        elif kind == "button":
            img(o, "VCVBezel", x, y) if HAVE_CL else o.insert(-1, f'<circle cx="{x}" cy="{y}" r="3" fill="#555"/>')
            if n.startswith("MUTE") or n.startswith("SOLO"):
                n0 = len(o)
                p.text(x, y + 0.75, n[0], 2.1, "#d9dbe2", 800, "middle", 0)
                o.insert(-1, o.pop(n0))
        elif kind == "fader":
            img(o, "", x, y, os.path.join(tmp, "bg.svg"))
            i = int(re.sub(r"\D", "", n)) - 1
            yy = FADER_BOT - HANDLE_H / 2 - (FADER_BOT - FADER_TOP - HANDLE_H) * POS[i]
            img(o, "", x, yy, os.path.join(tmp, "hd.svg"))
        else:
            img(o, "PJ301M", x, y) if HAVE_CL else o.insert(-1, f'<circle cx="{x}" cy="{y}" r="3" fill="#ccc"/>')
    for sx_ in (7.62, W - 7.62):
        for sy in (2.54, H - 2.54):
            img(o, "ScrewBlack", sx_, sy) if HAVE_CL else None
    # mock strip displays and meters
    for i in range(8):
        x, y, w, h = p.areas[f"SCRIB{i + 1}"]
        o.insert(-1, f'<rect x="{x}" y="{y}" width="{w}" height="1.3" rx="0.4" fill="{COLS[i]}"/>')
        p.out = o
        nm, ch = NAMES[k][i], (33 + i + 8 * k)
        ch = 1 + i + 8 * k
        chs = f"CH {ch}"
        for (s, yy, sz, c, wt) in ((nm, y + 6.0, 2.3, "#ffffff", 800), (ICONS[i], y + 8.4, 1.3, DIM, 600),
                                   (chs, y + 10.4, 1.3, "#5d606b", 600)):
            n0 = len(o)
            p.text(x + w / 2, yy, s, sz, c, wt, "middle", 0.15)
            o.insert(-1, o.pop(n0))
        x, y, w, h = p.areas[f"VU{i + 1}"]
        lit = int(12 * POS[i] * 0.9)
        seg = h / 12
        for s in range(12):
            c = "#ff4f4f" if s >= 10 else ("#ffc24d" if s >= 8 else "#5dff8a")
            o.insert(-1, f'<rect x="{x + 0.15:.2f}" y="{y + h - (s + 1) * seg + 0.3:.2f}" width="0.9" height="{seg - 0.6:.2f}" '
                         f'rx="0.2" fill="{c if s < lit else "#2a2b33"}"/>')
    if k == 0:
        for c in (0, 1):
            x, y, w, h = p.areas[f"SD{c + 1}"]
            rec = c == 0
            o.insert(-1, f'<path d="M{x} {y + 0.6} q0 -0.6 0.6 -0.6 h{w - 3.2} l2.6 2.6 v{h - 3.2} q0 0.6 -0.6 0.6 h{-(w - 1.2)} '
                         f'q-0.6 0 -0.6 -0.6 z" fill="#2a2c33" stroke="{"#ff4f4f" if rec else "#4a4c57"}" stroke-width="0.35"/>')
            for q in range(4):
                o.insert(-1, f'<rect x="{x + 1.0 + q * 1.35:.2f}" y="{y + 1.0}" width="0.9" height="1.8" rx="0.2" fill="#4a4c57"/>')
            o.insert(-1, f'<circle cx="{x + w / 2:.2f}" cy="{y + 6.0}" r="1.7" fill="{"#ff4f4f" if rec else "#3a3c44"}"/>')
            n0 = len(o)
            p.text(x + w / 2, y + 9.6, "12:04" if rec else "3:05 h", 1.25, "#ff4f4f" if rec else TXT, 700, "middle", 0.1)
            o.insert(-1, o.pop(n0))
        x, y, w, h = p.areas["STEP"]
        n0 = len(o)
        p.text(x + w / 2, y + 4.4, "1", 3.0, "#ffffff", 800, "middle", 0)
        o.insert(-1, o.pop(n0))
        tx, ty, dx, _ = p.areas["TRACKS"]
        o.insert(-1, f'<circle cx="{tx}" cy="{ty}" r="1.35" fill="{ACC}"/>')
    for b in (1, 2):
        x, y, w, h = p.areas[f"BUSLBL{b}"]
        n0 = len(o)
        p.text(x + w, y + 1.8, f"› BUS {b}", 1.3, ACC, 700, "end", 0.15)
        o.insert(-1, o.pop(n0))
    if k == 1:
        x, y, w, h = p.areas["STEP"]
        n0 = len(o)
        p.text(x + w / 2, y + 4.4, "8", 3.0, "#ffffff", 800, "middle", 0)
        o.insert(-1, o.pop(n0))
    x, y, w, h = p.areas["DISPLAY"]
    lines = ([("CH 1 – 8", 3.4, "#ffffff", 800, 4.6), ("● WING · MIDI OK · SYSEX OK · SD1 REC", 1.6, "#b8f23a", 700, 8.1),
              ("AUDIO → USB 33–48 · ASIO WING · 48 kHz", 1.5, DIM, 600, 10.2)] if k == 0 else
             [("CH 9 – 16", 3.4, "#ffffff", 800, 4.6), ("● LINKED TO WING-LINK", 1.6, ACC, 700, 8.1),
              ("AUDIO → USB 9–16", 1.5, DIM, 600, 10.2)])
    for s, sz, c, wt, dy in lines:
        n0 = len(o)
        p.text(x + 3, y + dy, s, sz, c, wt, "start", 0.2)
        o.insert(-1, o.pop(n0))
both = ['<svg xmlns="http://www.w3.org/2000/svg" width="{0:.2f}mm" height="{1}mm" viewBox="0 0 {0:.2f} {1}">'.format(2 * W, H)]
both.append(f'<svg x="0" y="0" width="{W:.2f}" height="{H}" viewBox="0 0 {W:.2f} {H}">' + "\n".join(main.out[1:]))
both.append(f'<svg x="{W:.2f}" y="0" width="{W:.2f}" height="{H}" viewBox="0 0 {W:.2f} {H}">' + "\n".join(xp.out[1:]))
both.append("</svg>")
svg = sys.argv[2].rsplit(".", 1)[0] + ".svg"
open(svg, "w").write("\n".join(both))
subprocess.run(["rsvg-convert", "-w", "2400", svg, "-o", sys.argv[2]], check=True)
print("wrote", sys.argv[2])
