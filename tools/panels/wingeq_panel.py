#!/usr/bin/env python3
"""Panel of WING-EQ, the expander for a WING channel's EQ (in the look of the GENRA plugin).

The channel (display: a click picks it, ◀ ▶ page through CH 1–40 and AUX 1–8), EQ ON and FLAT; the curve; six bands
(LOW and HIGH: shelf or peak; BAND 1–4) with GAIN, FREQ, Q and CV for each; the channel's LO CUT and HI CUT.

Usage:  wingeq_panel.py real                   -> res/WingEq.svg + src/WingEqLayout.hpp
        wingeq_panel.py preview OUT.png [RACK] -> preview with Rack components and a mock display
"""
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
if PREVIEW_RACK := (sys.argv[3] if len(sys.argv) > 3 else None):
    os.environ["RACK"] = PREVIEW_RACK
import panelkit as wl  # noqa: E402  (fonts, colours, Panel, img)

MODE = sys.argv[1] if len(sys.argv) > 1 else "real"
PREVIEW = MODE == "preview"
ROOT = wl.ROOT

HP = 30
W, H = HP * 5.08, 128.5
X0, CW = 5.4, 141.6 / 7          # the seven columns: LOW, BAND 1–4, HIGH, FILTER
BANDS = [("LOW", "#ff6b5b"), ("BAND 1", "#ff9a3e"), ("BAND 2", "#f0d040"), ("BAND 3", "#7ee08a"),
         ("BAND 4", "#3dc9ff"), ("HIGH", "#b48cff")]


class EqPanel(wl.Panel):
    def header(self, title, subtitle):
        self.e(f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{W:.2f}mm" height="{H}mm" viewBox="0 0 {W:.2f} {H}">')
        self.e(f'<rect width="{W:.2f}" height="{H}" fill="{wl.BG}"/>')
        self.text(12.9, 7.6, title, 5.1, wl.TITLE, 800, "start", 1.08)
        self.text(12.9 + wl.width(title, 5.1, 800, 1.08) + 3.4, 7.45, subtitle, 1.76, wl.DIM, 600, "start", 0.47)

    def mknob(self, name, x, y):
        self.comps.append(("medium", name, x, y))
        self.e(f'<circle cx="{x:.2f}" cy="{y}" r="{4.8 + 0.88:.2f}" fill="none" stroke="{wl.RING}" stroke-width="0.29" '
               f'stroke-dasharray="0.29 0.73" stroke-linecap="round"/>')

    def sbutton(self, name, x, y):
        self.comps.append(("sbutton", name, x, y))

    def tri(self, x, y, left):
        d = -1 if left else 1
        self.e(f'<path d="M{x - 0.8 * d:.2f} {y - 0.9:.2f} L{x + 0.8 * d:.2f} {y:.2f} L{x - 0.8 * d:.2f} {y + 0.9:.2f} Z" '
               f'fill="{wl.TXT}"/>')


p = EqPanel("eq")
p.header("WING-EQ", "EXPANDER · CHANNEL EQ")
DISP = (X0, 10.6, 92.0, 11.2)
p.e(f'<rect x="{DISP[0]}" y="{DISP[1]}" width="{DISP[2]}" height="{DISP[3]}" rx="1" fill="{wl.DARK}" '
    f'stroke="{wl.LINE}" stroke-width="0.3"/>')
p.area("DISPLAY", *DISP)
BY = 15.4
p.button("PREV", 105.0, BY)
p.tri(105.0, 21.2, True)
p.button("NEXT", 117.0, BY)
p.tri(117.0, 21.2, False)
p.button("EQ_ON", 131.0, BY)
p.text(131.0, 21.6, "ON", 1.4, wl.TXT, 800, "middle", 0.25)
p.button("FLAT", 144.0, BY)
p.text(144.0, 21.6, "FLAT", 1.4, wl.TXT, 800, "middle", 0.25)

CURVE = (X0, 24.0, 141.6, 33.0)
p.e(f'<rect x="{CURVE[0]}" y="{CURVE[1]}" width="{CURVE[2]}" height="{CURVE[3]}" rx="1" fill="{wl.DARK}" '
    f'stroke="{wl.LINE}" stroke-width="0.3"/>')
p.area("CURVE", *CURVE)

PY, PH = 59.5, 64.0
for i in range(7):
    x0 = X0 + i * CW
    cx = x0 + CW / 2
    name, col = BANDS[i] if i < 6 else ("FILTER", "#8a8d99")
    p.e(f'<rect x="{x0 + 0.4:.2f}" y="{PY}" width="{CW - 0.8:.2f}" height="{PH}" rx="1" fill="{wl.PLATE}"/>')
    p.e(f'<rect x="{x0 + 0.4:.2f}" y="{PY}" width="{CW - 0.8:.2f}" height="0.75" rx="0.35" fill="{col}"/>')
    p.text(x0 + 1.8, 64.2, name, 1.65, col if i < 6 else wl.TXT, 800, "start", 0.35)
    if i < 6:
        n = i + 1
        if i in (0, 5):
            p.sbutton(f"SHV{n}", x0 + CW - 4.0, 63.7)
            p.text(x0 + CW - 7.6, 64.2, "SHV", 1.2, wl.DIM, 800, "end", 0.2)
        p.mknob(f"GAIN{n}", cx, 73.1)
        p.text(cx, 80.9, "GAIN", 1.45, wl.TXT, 800, "middle", 0.3)
        p.area(f"VGAIN{n}", cx - 9.0, 81.4, 18.0, 2.4)
        p.knob(f"FREQ{n}", cx - 4.6, 89.2)
        p.knob(f"Q{n}", cx + 4.6, 89.2)
        p.text(cx - 4.6, 94.4, "FREQ", 1.25, wl.TXT, 800, "middle", 0.2)
        p.text(cx + 4.6, 94.4, "Q", 1.25, wl.TXT, 800, "middle", 0.2)
        p.area(f"VFREQ{n}", cx - 9.0, 94.9, 9.0, 2.2)
        p.area(f"VQ{n}", cx, 94.9, 9.0, 2.2)
        p.e(f'<line x1="{x0 + 2:.2f}" y1="99" x2="{x0 + CW - 2:.2f}" y2="99" stroke="{wl.LINE}" stroke-width="0.3"/>')
        p.jack(f"CV_GAIN{n}", cx - 4.6, 104.6, "GAIN")
        p.jack(f"CV_FREQ{n}", cx + 4.6, 104.6, "FREQ")
        p.jack(f"CV_Q{n}", cx, 116.2, "Q")
    else:
        p.sbutton("LC_ON", cx - 5.2, 71.0)
        p.knob("LCF", cx + 4.0, 71.0)
        p.text(cx, 78.0, "LO CUT", 1.3, wl.TXT, 800, "middle", 0.2)
        p.area("VLCF", cx - 9.0, 78.5, 18.0, 2.2)
        p.sbutton("HC_ON", cx - 5.2, 87.0)
        p.knob("HCF", cx + 4.0, 87.0)
        p.text(cx, 94.0, "HI CUT", 1.3, wl.TXT, 800, "middle", 0.2)
        p.area("VHCF", cx - 9.0, 94.5, 18.0, 2.2)
        p.e(f'<line x1="{x0 + 2:.2f}" y1="99" x2="{x0 + CW - 2:.2f}" y2="99" stroke="{wl.LINE}" stroke-width="0.3"/>')
        p.jack("CV_LC", cx - 4.6, 104.6, "LO CUT")
        p.jack("CV_HC", cx + 4.6, 104.6, "HI CUT")
        p.jack("CV_ON", cx, 116.2, "EQ ON")
p.finish()

if not PREVIEW:
    open(os.path.join(ROOT, "res", "WingEq.svg"), "w").write("\n".join(p.out))
    lines = ["// Generated by tools/panels/wingeq_panel.py - do not edit", "#pragma once", "namespace eqlayout {"]
    for kind, n, x, y in p.comps:
        lines.append(f"static const float {n}[2] = {{{x:.3f}f, {y:.3f}f}};")
    for n, (x, y, w, h) in p.areas.items():
        lines.append(f"static const float {n}[4] = {{{x:.3f}f, {y:.3f}f, {w:.3f}f, {h:.3f}f}};")
    lines += [f"static const int HP = {HP};", "}"]
    open(os.path.join(ROOT, "src", "WingEqLayout.hpp"), "w").write("\n".join(lines) + "\n")
    print("wrote res/WingEq.svg and src/WingEqLayout.hpp")
    sys.exit(0)

# ---------------------------------------------------------------- preview
o = p.out
KNOB = os.path.join(ROOT, "res", "knob")
for kind, n, x, y in p.comps:
    if kind in ("small", "medium"):
        wl.img(o, "", x, y, os.path.join(KNOB, kind + "_bg.svg"))
        wl.img(o, "", x, y, os.path.join(KNOB, kind + ".svg"))
    elif kind == "button":
        wl.img(o, "VCVBezel", x, y)
    elif kind == "sbutton":
        wl.img(o, "VCVButton_0", x, y)
    else:
        wl.img(o, "PJ301M", x, y)
for sx_ in (7.62, W - 7.62):
    for sy in (2.54, H - 2.54):
        wl.img(o, "ScrewBlack", sx_, sy)


def put(s, x, y, size, c, wt, anchor="middle", sp=0.15):
    n0 = len(o)
    p.text(x, y, s, size, c, wt, anchor, sp)
    if len(o) > n0:
        o.insert(-1, o.pop(n0))


x, y, w, h = p.areas["DISPLAY"]
o.insert(-1, f'<rect x="{x + 2.6}" y="{y + 1.8}" width="1.1" height="7.4" rx="0.4" fill="#ff6b5b"/>')
put("CH 1 · KICK", x + 5.4, y + 5.5, 3.2, "#ffffff", 800, "start", 0.4)
put("●  FOLLOWS STRIP 1 OF WING-LINK  ·  EQ ON  ·  STD", x + 5.4, y + 8.9, 1.45, wl.ACCENT if hasattr(wl, "ACCENT") else "#7ee08a", 700, "start", 0.3)
# the curve (a mock: the module draws it from the knobs)
gx, gy, gw, gh = p.areas["CURVE"]
px0, px1, mid = gx + 7, gx + gw - 3, gy + gh / 2
dbs = gh * 0.42 / 15


def fx(f):
    return px0 + (math.log10(f) - math.log10(20)) / 3 * (px1 - px0)


MOCK = [("L", 80, 4.0, 0.7), ("1", 200, -3.0, 1.2), ("2", 600, -4.5, 2.0), ("3", 2500, 2.5, 1.0), ("4", 5000, 3.5, 1.5),
        ("H", 10000, 2.0, 0.7)]


def resp(f, b):
    n, fc, g, q = b
    xx = math.log2(f / fc)
    if n not in "LH":
        return g * math.exp(-(xx * q * 1.2) ** 2)
    return g / (1 + math.exp((xx if n == "L" else -xx) * 3))


for fq in (20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000):
    o.insert(-1, f'<line x1="{fx(fq):.2f}" y1="{gy + 2}" x2="{fx(fq):.2f}" y2="{gy + gh - 3.5}" stroke="#1d1f25" stroke-width="0.25"/>')
pts = []
for k in range(161):
    fq = 20 * 10 ** (3 * k / 160)
    d = sum(resp(fq, b) for b in MOCK)
    pts.append(f"{fx(fq):.2f},{mid - d * dbs:.2f}")
o.insert(-1, f'<polyline points="{" ".join(pts)}" fill="none" stroke="#f2f3f7" stroke-width="0.5"/>')
for (n, fc, g, q), (_, col) in zip(MOCK, BANDS):
    yy = mid - sum(resp(fc, b) for b in MOCK) * dbs
    o.insert(-1, f'<circle cx="{fx(fc):.2f}" cy="{yy:.2f}" r="1.5" fill="{col}"/>')
svg = sys.argv[2].rsplit(".", 1)[0] + ".svg"
open(svg, "w").write("\n".join(o))
subprocess.run(["rsvg-convert", "-w", "1700", svg, "-o", sys.argv[2]], check=True)
os.remove(svg)
print("wrote", sys.argv[2])
